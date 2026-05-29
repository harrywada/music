#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <unistd.h>
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

struct queue_ctx {
	const char *sockpath;
	bool        has_pos;
	long        idx; /* current insert_at index; mutated per song */
	bool        neg; /* true only when size query failed; uses fixed idx */
};

static int
queue_song(const char *path, unsigned long uid, void *ud)
{
	struct queue_ctx *ctx = ud;
	int sock = connect_socket(ctx->sockpath);
	if (sock == -1) return 0;
	if (ctx->has_pos)
		dprintf(sock, "insert %ld %s#%lu\n", ctx->idx, path, uid);
	else
		dprintf(sock, "queue %s#%lu\n", path, uid);
	close(sock);
	if (ctx->has_pos && !ctx->neg)
		ctx->idx++;
	return 1;
}

/*
 * Query the daemon for the current queue length.
 * Returns the length on success, or -1 on failure.
 */
static long
query_qsize(const char *sockpath)
{
	int sock = connect_socket(sockpath);
	if (sock == -1)
		return -1;
	dprintf(sock, "size\n");
	char    buf[32];
	ssize_t len = read(sock, buf, sizeof buf - 1);
	close(sock);
	if (len <= 0)
		return -1;
	buf[len] = '\0';
	char *end;
	long  n = strtol(buf, &end, 10);
	if (*end != '\n' && *end != '\0')
		return -1;
	return n;
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

	/*
	 * For a negative position, resolve it to an absolute index now so
	 * that all songs are inserted in order.  The fixed-negative-idx
	 * approach only works when the queue is large enough; if n is too
	 * small the same back-offset wraps back before the last inserted
	 * song.  Querying the size once and then incrementing gives correct
	 * ordering in all cases.
	 *
	 * Fall back to the old fixed-idx approach only when the size query
	 * fails; in that case insert_at's own clamping (pos >= 1) at least
	 * prevents displacing the active song.
	 */
	long initial_idx;
	bool neg;

	if (has_pos && pos < 0) {
		long n = query_qsize(sockpath);
		if (n >= 0) {
			/* n + pos = n - |pos|, clamped to [1, n]. */
			long start = n + pos;
			initial_idx = start < 1 ? 1 : start;
			neg         = false;
		} else {
			initial_idx = pos - 1;
			neg         = true;
		}
	} else {
		initial_idx = has_pos ? pos + 1 : 0;
		neg         = false;
	}

	struct queue_ctx ctx = {
		.sockpath = sockpath,
		.has_pos  = has_pos,
		.idx      = initial_idx,
		.neg      = neg,
	};

	char  *line    = NULL;
	size_t linecap = 0;
	ssize_t len;

	while ((len = getline(&line, &linecap, stdin)) > 0) {
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = '\0';
		if (!len) continue;
		expand_song(line, queue_song, &ctx);
	}

	free(line);
	return 0;
}
