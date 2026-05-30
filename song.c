#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "ebml.h"
#include "matroska.h"
#include "matroska_utils.h"
#include "song.h"
#include "utils.h"

bool parse_song(const char *line, struct song *s)
{
	const char *end = strchr(line, '#');
	if (!end)
		return false;

	s->path = malloc((end - line + 1) * sizeof(char));
	if (!s->path)
		return false;
	memcpy(s->path, line, end - line);
	s->path[end - line] = '\0';
	
	char *uid_end;
	if (!isdigit(*++end))
		goto err;

	s->uid = strtoul(end, &uid_end, 10);
	if (s->uid == ULONG_MAX) {
		debug(errno, "strtoul");
		goto err;
	} else if (*uid_end != '\0') {
		goto err;
	}

	return true;

err:
	free(s->path);
	return false;
}

void
cleanup_song(struct song *s)
{
	free(s->path);
}

struct expand_ctx {
	const char *path;
	int (*cb)(const char *, unsigned long, void *);
	void *ud;
	int   n;
};

static int
expand_visit(const struct mkv_chapter *ch, void *ud)
{
	struct expand_ctx *ctx = ud;
	ctx->n++;
	return ctx->cb(ctx->path, (unsigned long)ch->uid, ctx->ud);
}

int
expand_song(const char *line,
            int (*cb)(const char *, unsigned long, void *),
            void *ud)
{
	if (strchr(line, '#')) {
		struct song s;
		if (!parse_song(line, &s)) {
			warn(0, "%s: malformed song reference", line);
			return 0;
		}
		cb(s.path, s.uid, ud);
		cleanup_song(&s);
		return 1;
	}

	int fd = open(line, O_RDONLY);
	if (fd == -1) { warn(errno, "%s", line); return 0; }

	struct mkv_seekinfo si = {0};
	if (ebml_skip(fd, EBML_HEADER) == -1
	||  ebml_descend(fd, MKV_SEGMENT) == -1
	||  !mkv_readseekinfo(fd, &si)) {
		warn(0, "%s: can't read Matroska headers", line);
		close(fd);
		return 0;
	}
	if (!si.chapters) {
		warn(0, "%s: no chapters", line);
		close(fd);
		return 0;
	}

	seek(fd, si.segment + si.chapters);
	struct expand_ctx ctx = { .path = line, .cb = cb, .ud = ud };
	mkv_visitchapters(fd, expand_visit, &ctx);
	close(fd);
	return ctx.n;
}
