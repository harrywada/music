#pragma once

#include <stddef.h>
#include "song.h"

struct queue {
	unsigned int size;
	/* NOTE Elements are indexed as `[head, tail).` */
	unsigned int head, tail;
	struct song *data;
};

bool mkqueue(struct queue *, unsigned int);
void cleanup_queue(struct queue *);

struct queue insert_at(struct queue, struct song, int);
struct queue pop(struct queue, struct song *);
struct queue push(struct queue, struct song);
[[gnu::const]]
unsigned int qsize(struct queue);
