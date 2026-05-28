#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <unistd.h>
#include "ebml.h"
#include "matroska.h"
#include "song.h"
#include "utils.h"

static int
connect_socket(const char *path)
{
	struct sockaddr_un addr = {0};
	int fd;

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		warn(errno, "socket");
		return -1;
	}
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof addr.sun_path - 1);
	if (connect(fd, (struct sockaddr *)&addr, sizeof addr) == -1) {
		warn(errno, "connect: %s", path);
		close(fd);
		return -1;
	}
	return fd;
}

struct expand_ctx {
	const char *sockpath;
	const char *filepath;
	bool        has_pos;
	long        idx; /* current insert_at index; mutated per song */
	bool        neg; /* true when sq position < 0 */
};

static int
queue_chapter(const struct mkv_chapter *ch, void *ud)
{
	struct expand_ctx *ctx = ud;
	int sock = connect_socket(ctx->sockpath);
	if (sock == -1) return 0;
	if (ctx->has_pos)
		dprintf(sock, "insert %s#%lu %ld\n",
		        ctx->filepath, (unsigned long) ch->uid, ctx->idx);
	else
		dprintf(sock, "queue %s#%lu\n",
		        ctx->filepath, (unsigned long) ch->uid);
	close(sock);
	if (ctx->has_pos && !ctx->neg)
		ctx->idx++;
	return 1;
}

int
main(int argc, char *argv[])
{
	const char *sockpath = NULL;
	bool        has_pos  = false;
	long        pos      = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
			sockpath = argv[++i];
		} else if (!has_pos) {
			char *end;
			pos = strtol(argv[i], &end, 10);
			if (*end != '\0') {
				fprintf(stderr,
				        "Usage: sq -s <socket-path> [position]\n");
				return 1;
			}
			has_pos = true;
		} else {
			fprintf(stderr, "Usage: sq -s <socket-path> [position]\n");
			return 1;
		}
	}
	if (!sockpath) {
		fprintf(stderr, "Usage: sq -s <socket-path> [position]\n");
		return 1;
	}

	long cur_idx = has_pos ? (pos >= 0 ? pos + 1 : pos - 1) : 0;
	bool neg     = has_pos && pos < 0;

	char  *line    = NULL;
	size_t linecap = 0;
	ssize_t len;

	while ((len = getline(&line, &linecap, stdin)) > 0) {
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = '\0';
		if (!len) continue;

		if (strchr(line, '#')) {
			/* path#uid — send directly. */
			int sock = connect_socket(sockpath);
			if (sock == -1) continue;
			if (has_pos) {
				dprintf(sock, "insert %s %ld\n", line, cur_idx);
				if (!neg) cur_idx++;
			} else {
				dprintf(sock, "queue %s\n", line);
			}
			close(sock);
		} else {
			/* Bare path — expand all chapters. */
			int fd = open(line, O_RDONLY);
			if (fd == -1) { warn(errno, "%s", line); continue; }

			struct mkv_seekinfo si = {0};
			if (ebml_skip(fd, EBML_HEADER) == -1
			||  ebml_descend(fd, MKV_SEGMENT) == -1
			||  !mkv_readseekinfo(fd, &si)) {
				warn(0, "sq: can't read %s", line);
				close(fd);
				continue;
			}

			if (si.chapters == 0) {
				warn(0, "sq: no chapters in %s", line);
				close(fd);
				continue;
			}

			seek(fd, si.segment + si.chapters);
			struct expand_ctx ctx = { .sockpath = sockpath,
			                          .filepath = line,
			                          .has_pos  = has_pos,
			                          .idx      = cur_idx,
			                          .neg      = neg };
			mkv_visitchapters(fd, queue_chapter, &ctx);
			cur_idx = ctx.idx;
			close(fd);
		}
	}

	free(line);
	return 0;
}
