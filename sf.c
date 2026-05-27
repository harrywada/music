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
#include "filter.h"
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
		warn(0, "sf: can't read Matroska headers in %s", path);
		close(fd);
		return -1;
	}
	return fd;
}

static void
eval_song(const char *path, unsigned long uid,
          const struct filter_node *filter)
{
	int fd;
	struct mkv_seekinfo si;
	struct mkv_chapter  chapter;
	struct mkv_track    track;
	struct song_tags    tags = {0};
	bool print = false;

	if ((fd = open_mkv(path, &si)) == -1) return;

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
}

struct sf_ctx {
	const char              *path;
	int                      fd;
	struct mkv_seekinfo      si;
	const struct filter_node *filter;
	bool                     have_track; /* audio track pre-fetched below */
	struct mkv_track         track;
};

static int
sf_chapter_cb(const struct mkv_chapter *ch, void *ud)
{
	struct sf_ctx   *ctx = ud;
	struct mkv_track track;
	struct song_tags tags = {0};
	bool print = false;

	/* Use the pre-fetched audio track; fall back to per-chapter lookup
	   only when the file has no cached track (should not normally happen). */
	if (ctx->have_track) {
		track = ctx->track;
	} else {
		if (!ctx->si.tracks) goto done;
		seek(ctx->fd, ctx->si.segment + ctx->si.tracks);
		if (!mkv_findtrack(ctx->fd, ch->track_uids, &track)) goto done;
	}

	if (ctx->si.tags) {
		seek(ctx->fd, ctx->si.segment + ctx->si.tags);
		mkv_readsongtags(ctx->fd, ch->uid, track.uid, &tags);
	}

	print = !ctx->filter || filter_eval(ctx->filter, &tags);

done:
	song_tags_free(&tags);
	if (print)
		printf("%s#%lu\n", ctx->path, (unsigned long)ch->uid);
	return 1;
}

static void
eval_file(const char *path, const struct filter_node *filter)
{
	struct mkv_seekinfo si;
	int fd;

	if ((fd = open_mkv(path, &si)) == -1) return;

	if (!si.chapters) {
		warn(0, "sf: no chapters in %s", path);
		close(fd);
		return;
	}

	/* Pre-fetch the first audio track once; all chapters in a music
	   container share a single audio track, so this avoids re-reading
	   the Tracks section for every chapter. */
	static const uint64_t zero_uids[TRACKS_MAX];
	struct sf_ctx ctx = { .path = path, .fd = fd, .si = si,
	                      .filter = filter };
	if (si.tracks) {
		seek(fd, si.segment + si.tracks);
		ctx.have_track = mkv_findtrack(fd, zero_uids, &ctx.track);
	}

	seek(fd, si.segment + si.chapters);
	mkv_visitchapters(fd, sf_chapter_cb, &ctx);
	close(fd);
}

int
main(int argc, char *argv[])
{
	int err = 0;
	struct filter_node *filter =
		filter_parse(argc - 1, (const char **)(argv + 1), &err);
	if (err) return 1;

	char   *line    = NULL;
	size_t  linecap = 0;
	ssize_t len;

	while ((len = getline(&line, &linecap, stdin)) > 0) {
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = '\0';
		if (!len) continue;

		if (strchr(line, '#')) {
			struct song song;
			if (!parse_song(line, &song)) {
				warn(0, "sf: invalid song: %s", line);
				continue;
			}
			eval_song(song.path, song.uid, filter);
			cleanup_song(&song);
		} else {
			eval_file(line, filter);
		}
	}

	free(line);
	filter_free(filter);
	return 0;
}
