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
	bool        replace_active;
	bool        replace_all;
	bool        initialized;
	long        idx; /* current insert_at index; mutated per song */
	bool        neg; /* true when sq position < 0 */
};

static bool
send_cmd(const char *sockpath, const char *cmd)
{
	int sock = connect_socket(sockpath);
	if (sock == -1)
		return false;
	dprintf(sock, "%s\n", cmd);
	close(sock);
	return true;
}

static bool
init_queue(struct queue_ctx *ctx)
{
	if (ctx->initialized)
		return true;

	if (ctx->replace_all) {
		const char *cmd = ctx->replace_active ? "clearall" : "clear";
		if (!send_cmd(ctx->sockpath, cmd))
			return false;
	} else if (ctx->replace_active) {
		if (!send_cmd(ctx->sockpath, "stop"))
			return false;
	}

	ctx->initialized = true;
	return true;
}

static ssize_t
read_song_line(char **line, size_t *linecap)
{
	ssize_t len;

	while ((len = getline(line, linecap, stdin)) > 0) {
		while (len > 0
		    && ((*line)[len - 1] == '\n' || (*line)[len - 1] == '\r'))
			(*line)[--len] = '\0';
		if (len)
			return len;
	}

	return len;
}

static int
queue_song(const char *path, unsigned long uid, void *ud)
{
	struct queue_ctx *ctx = ud;
	if (!init_queue(ctx))
		return 0;

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

int
main(int argc, char *argv[])
{
	const char *sockpath = NULL;
	bool        has_pos  = false;
	bool        clear    = false;
	bool        include0 = false;
	long        pos      = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
			sockpath = argv[++i];
		} else if (strcmp(argv[i], "-x") == 0) {
			clear = true;
		} else if (strcmp(argv[i], "-0") == 0) {
			include0 = true;
		} else if (!has_pos) {
			char *end;
			pos = strtol(argv[i], &end, 10);
			if (*end != '\0') {
				fprintf(stderr,
				        "Usage: sq -s <socket-path> [-x] [-0] [position]\n");
				return 1;
			}
			has_pos = true;
		} else {
			fprintf(stderr, "Usage: sq -s <socket-path> [-x] [-0] [position]\n");
			return 1;
		}
	}
	if (!sockpath) {
		fprintf(stderr, "Usage: sq -s <socket-path> [-x] [-0] [position]\n");
		return 1;
	}

	struct queue_ctx ctx = {
		.sockpath       = sockpath,
		.has_pos        = has_pos,
		.replace_active = include0,
		.replace_all    = clear,
		.initialized    = false,
		.idx            = has_pos ? (include0 ? pos :
		                  (pos >= 0 ? pos + 1 : pos - 1)) : 0,
		.neg            = has_pos && pos < 0,
	};

	char  *line    = NULL;
	size_t linecap = 0;
	ssize_t len = read_song_line(&line, &linecap);

	if (len <= 0) {
		fprintf(stderr, "sq: no songs on standard input\n");
		free(line);
		return 0;
	}

	do {
		expand_song(line, queue_song, &ctx);
	} while ((len = read_song_line(&line, &linecap)) > 0);

	free(line);
	return 0;
}
