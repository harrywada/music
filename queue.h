#pragma once

#include <stddef.h>
#include "song.h"

struct queue {
	unsigned int size;
	/* NOTE Elements are indexed as `[head, tail).` */
	unsigned int head, tail;
	[[clang::sized_by(size)]]
	struct song *data;
};

struct queue pop(struct queue, struct song *);
struct queue push(struct queue, struct song);
[[gnu::const]]
unsigned int qsize(struct queue);

void cleanup_queue(struct queue *);
