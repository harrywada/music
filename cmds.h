#pragma once

#include "state.h"

struct state cmd(struct state, unsigned int, const char *[]);

#define CMD_DECL(cmd)  \
	[[gnu::const]] \
	struct state cmd_ ## cmd(struct state, unsigned int, const char *[])
CMD_DECL(exit);
CMD_DECL(loop);
CMD_DECL(consume);
CMD_DECL(pause);
CMD_DECL(play);
CMD_DECL(skip);
CMD_DECL(stop);
CMD_DECL(toggle);
#undef CMD_DECL

/* These log on argument errors, so they cannot be [[gnu::const]]. */
struct state cmd_queue(struct state, unsigned int, const char *[]);
struct state cmd_insert(struct state, unsigned int, const char *[]);
struct state cmd_clear(struct state, unsigned int, const char *[]);

void cmd_list(struct state, int);
void cmd_size(struct state, int);
void cmd_status(struct state, int);
