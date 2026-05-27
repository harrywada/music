#pragma once

#include "queue.h"

enum mode {
	CONSUME,
	EXITING,
	LOOP,
	SHUFFLE,
};

enum playstate {
	PLAYING,
	PAUSED,
	STOPPED,
};

struct state {
	enum mode mode;
	struct queue queue;
	unsigned int cur;
	enum playstate play;
};

[[gnu::nonnull(1)]]
bool mkstate(struct state *);
[[gnu::nonnull(1)]]
void cleanup_state(struct state *);
