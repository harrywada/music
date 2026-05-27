#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include "ebml.h"
#include "matroska.h"
#include "format.h"
#include "song.h"
#include "utils.h"

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
		warn(0, "sp: can't read Matroska headers in %s", path);
		close(fd);
		return -1;
	}
	return fd;
}

static void
eval_song(const char *path, unsigned long uid,
          const struct format *fmt)
{
	int fd;
	struct mkv_seekinfo si;
	struct mkv_chapter  chapter;
	struct mkv_track    track;
	struct song_tags    tags = {0};

	if ((fd = open_mkv(path, &si)) == -1) return;

	if (!si.chapters) {
		warn(0, "sp: no chapters in %s", path);
		goto done;
	}

	seek(fd, si.segment + si.chapters);
	if (!mkv_findchapter(fd, uid, &chapter)) {
		warn(0, "sp: chapter %lu not found in %s", uid, path);
		goto done;
	}

	if (!si.tracks) {
		warn(0, "sp: no tracks in %s", path);
		goto done;
	}

	seek(fd, si.segment + si.tracks);
	if (!mkv_findtrack(fd, chapter.track_uids, &track)) {
		warn(0, "sp: no audio track in %s", path);
		goto done;
	}

	if (si.tags) {
		seek(fd, si.segment + si.tags);
		mkv_readsongtags(fd, uid, track.uid, &tags);
	}

	format_print(fmt, &tags, STDOUT_FILENO);

done:
	song_tags_free(&tags);
	close(fd);
}

struct sp_ctx {
	const char         *path;
	int                 fd;
	struct mkv_seekinfo si;
	const struct format *fmt;
};

static int
sp_chapter_cb(const struct mkv_chapter *ch, void *ud)
{
	struct sp_ctx   *ctx  = ud;
	struct mkv_track track;
	struct song_tags tags = {0};

	if (!ctx->si.tracks) goto done;

	seek(ctx->fd, ctx->si.segment + ctx->si.tracks);
	if (!mkv_findtrack(ctx->fd, ch->track_uids, &track)) goto done;

	if (ctx->si.tags) {
		seek(ctx->fd, ctx->si.segment + ctx->si.tags);
		mkv_readsongtags(ctx->fd, ch->uid, track.uid, &tags);
	}

	format_print(ctx->fmt, &tags, STDOUT_FILENO);

done:
	song_tags_free(&tags);
	return 1;
}

static void
eval_file(const char *path, const struct format *fmt)
{
	struct mkv_seekinfo si;
	int fd;

	if ((fd = open_mkv(path, &si)) == -1) return;

	if (!si.chapters) {
		warn(0, "sp: no chapters in %s", path);
		close(fd);
		return;
	}

	seek(fd, si.segment + si.chapters);
	struct sp_ctx ctx = { .path = path, .fd = fd, .si = si, .fmt = fmt };
	mkv_visitchapters(fd, sp_chapter_cb, &ctx);
	close(fd);
}

int
main(int argc, char *argv[])
{
	if (argc != 2) {
		fprintf(stderr, "Usage: sp <format>\n");
		return 1;
	}

	struct format *fmt = format_compile(argv[1]);
	if (!fmt) return 1;

	char   *line   = NULL;
	size_t  linecap = 0;
	ssize_t len;

	while ((len = getline(&line, &linecap, stdin)) > 0) {
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = '\0';
		if (!len) continue;

		if (strchr(line, '#')) {
			struct song song;
			if (!parse_song(line, &song)) {
				warn(0, "sp: invalid song: %s", line);
				continue;
			}
			eval_song(song.path, song.uid, fmt);
			cleanup_song(&song);
		} else {
			eval_file(line, fmt);
		}
	}

	free(line);
	format_free(fmt);
	return 0;
}
