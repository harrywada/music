#include <errno.h>
#include <stdlib.h>

#include "queue.h"
#include "utils.h"

static struct queue
requeue(struct queue oldq, unsigned int size)
{
	if (size < oldq.size) {
		debug(0, 0, "Bad queue size (%d < %d)", size, oldq.size);
		return oldq;
	}

	struct queue newq = { .head = 0, .tail = 0, .size = size };
	newq.data = malloc(qsize(newq) * sizeof(struct song) + 1);
	if (!newq.data) {
		debug(errno, "malloc");
		return oldq;
	}

	unsigned int i = 0;
	while (oldq.head != oldq.tail)
		newq.data[i++] = oldq.data[oldq.head++ % oldq.size];
	newq.tail = i;

	free(oldq.data);
	return newq;
}

struct queue
pop(struct queue q, struct song *dest)
{
	if (qsize(q) < 1)
		return q;

	*dest = q.data[q.head++ % q.size];
	return q;
}

struct queue
push(struct queue q, struct song s)
{
	while (q.size <= qsize(q))
		q = requeue(q, 2 * q.size);

	q.data[q.tail++ % q.size] = s;
	return q;
}

unsigned int
qsize(struct queue q)
{
	return q.tail - q.head;
}

void
cleanup_queue(struct queue *q)
{
	while (qsize(*q)) {
		[[gnu::cleanup(cleanup_song)]]
		struct song song;
		*q = pop(*q, &song);
	}

	free(q->data);
}
