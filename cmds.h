#pragma once

#include "state.h"

[[gnu::const]]
struct state cmd(struct state, unsigned int, const char *[]);

#define CMD_DECL(cmd)  \
	[[gnu::const]] \
	struct state cmd_ ## cmd(struct state, unsigned int, const char *[])
CMD_DECL(exit);
CMD_DECL(queue);
CMD_DECL(pause);
CMD_DECL(play);
CMD_DECL(insert);
CMD_DECL(skip);
CMD_DECL(stop);
CMD_DECL(toggle);
#undef CMD_DECL

void cmd_list(struct state, int);
