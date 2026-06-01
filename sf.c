#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "ebml.h"
#include "matroska.h"
#include "matroska_utils.h"
#include "tags.h"
#include "filter.h"
#include "song.h"
#include "utils.h"

static int eval_song(const char *, unsigned long, void *);

static char *
xstrdup(const char *s)
{
	char *dup = strdup(s);
	if (!dup)
		die(errno, "strdup");
	return dup;
}

static char *
xjoin(const char *a, const char *b)
{
	size_t alen = strlen(a);
	size_t blen = strlen(b);
	char *s = malloc(alen + blen + 1);
	if (!s)
		die(errno, "malloc");
	memcpy(s, a, alen);
	memcpy(s + alen, b, blen + 1);
	return s;
}

static char *
join_path(const char *dir, const char *name)
{
	char *slash = xjoin(dir, "/");
	char *path = xjoin(slash, name);
	free(slash);
	return path;
}

static char *
home_path(const char *path)
{
	const char *home = getenv("HOME");
	if (!home || !*home)
		return NULL;
	return join_path(home, path);
}

static char *
xdg_config_path(void)
{
	const char *config = getenv("XDG_CONFIG_HOME");
	if (config && *config)
		return join_path(config, "user-dirs.dirs");
	return home_path(".config/user-dirs.dirs");
}

static char *
expand_home(const char *s)
{
	const char *home = getenv("HOME");
	if (!home)
		home = "";
	if (strncmp(s, "$HOME", 5) == 0)
		return xjoin(home, s + 5);
	if (strncmp(s, "${HOME}", 7) == 0)
		return xjoin(home, s + 7);
	return xstrdup(s);
}

static char *
parse_xdg_value(char *line)
{
	char *p = strchr(line, '=');
	if (!p)
		return NULL;
	p++;

	if (*p == '"') {
		char *start = ++p;
		char *out = p;
		while (*p && *p != '"') {
			if (*p == '\\' && p[1])
				p++;
			*out++ = *p++;
		}
		*out = '\0';
		return expand_home(start);
	}

	char *end = p + strlen(p);
	while (end > p && (end[-1] == '\n' || end[-1] == '\r'))
		*--end = '\0';
	return expand_home(p);
}

static char *
music_dir(void)
{
	const char *env = getenv("XDG_MUSIC_DIR");
	if (env && *env)
		return expand_home(env);

	char *config = xdg_config_path();
	if (config) {
		FILE *fp = fopen(config, "r");
		free(config);
		if (fp) {
			char   *line = NULL;
			size_t  cap = 0;
			char   *path = NULL;
			while (getline(&line, &cap, fp) > 0) {
				if (strncmp(line, "XDG_MUSIC_DIR=", 14) == 0) {
					path = parse_xdg_value(line);
					break;
				}
			}
			free(line);
			fclose(fp);
			if (path)
				return path;
		}
	}

	return home_path("Music");
}

static void
process_path(const char *path, struct filter_node *filter)
{
	if (*path)
		expand_song(path, eval_song, filter);
}

static void
process_stdin(struct filter_node *filter)
{
	char   *line    = NULL;
	size_t  linecap = 0;
	ssize_t len;

	while ((len = getline(&line, &linecap, stdin)) > 0) {
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = '\0';
		process_path(line, filter);
	}

	free(line);
}

static void
process_music_dir(struct filter_node *filter)
{
	char *dirpath = music_dir();
	if (!dirpath) {
		warn(0, "sf: can't find music directory");
		return;
	}

	DIR *dir = opendir(dirpath);
	if (!dir) {
		warn(errno, "%s", dirpath);
		free(dirpath);
		return;
	}

	struct dirent *de;
	while ((de = readdir(dir))) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;

		char *path = join_path(dirpath, de->d_name);
		struct stat st;
		if (stat(path, &st) == -1) {
			warn(errno, "%s", path);
			free(path);
			continue;
		}
		if (S_ISREG(st.st_mode))
			process_path(path, filter);
		free(path);
	}

	if (closedir(dir) == -1)
		warn(errno, "%s", dirpath);
	free(dirpath);
}

/* Open a Matroska file, skip EBML header, descend into Segment,
   read seekinfo.  Returns fd on success, -1 on failure (already warned). */
static int
open_mkv(const char *path, struct mkv_seekinfo *si)
{
	int fd = open(path, O_RDONLY);
	if (fd == -1) { warn(errno, "%s", path); return -1; }

	*si = (struct mkv_seekinfo){0};
	if (ebml_skip(fd, EBML_HEADER) == -1
	||  ebml_descend(fd, MKV_SEGMENT) == -1
	||  !mkv_readseekinfo(fd, si)) {
		warn(0, "sf: can't read Matroska headers in %s", path);
		close(fd);
		return -1;
	}
	return fd;
}

static int
eval_song(const char *path, unsigned long uid, void *ud)
{
	const struct filter_node *filter = ud;
	int fd;
	struct mkv_seekinfo si;
	struct mkv_chapter  chapter;
	struct mkv_track    track;
	struct song_tags    tags = {0};
	bool print = false;

	if ((fd = open_mkv(path, &si)) == -1) return 1;

	if (!si.chapters) {
		warn(0, "sf: no chapters in %s", path);
		goto done;
	}

	seek(fd, si.segment + si.chapters);
	if (!mkv_findchapter(fd, uid, &chapter)) {
		warn(0, "sf: chapter %lu not found in %s", uid, path);
		goto done;
	}

	if (!si.tracks) {
		warn(0, "sf: no tracks in %s", path);
		goto done;
	}

	seek(fd, si.segment + si.tracks);
	if (!mkv_findtrack(fd, chapter.track_uids, &track)) {
		warn(0, "sf: no audio track in %s", path);
		goto done;
	}

	if (si.tags) {
		seek(fd, si.segment + si.tags);
		mkv_readsongtags(fd, uid, track.uid, &tags);
	}

	print = !filter || filter_eval(filter, &tags);

done:
	song_tags_free(&tags);
	close(fd);
	if (print)
		printf("%s#%lu\n", path, uid);
	return 1;
}

int
main(int argc, char *argv[])
{
	int err = 0;
	struct filter_node *filter =
		filter_parse(argc - 1, (const char **)(argv + 1), &err);
	if (err) return 1;

	if (isatty(STDIN_FILENO))
		process_music_dir(filter);
	else
		process_stdin(filter);

	filter_free(filter);
	return 0;
}
