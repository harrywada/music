#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "queue.h"
#include "utils.h"

static struct queue
requeue(struct queue oldq, unsigned int size)
{
	if (size < oldq.size) {
		debug(0, "Bad queue size (%d < %d)", size, oldq.size);
		return oldq;
	}

	struct queue newq = { .head = 0, .tail = 0, .size = size };
	newq.data = malloc(size * sizeof(struct song));
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

bool
mkqueue(struct queue *q, unsigned int size)
{
	*q = (struct queue) { .head = 0, .tail = 0, .size = 0, .data = nullptr };
	if ((*q = requeue(*q, size)).size != size)
		return false;

	return true;
}

struct queue
pop(struct queue q, struct song *dest)
{
	if (qsize(q) < 1)
		return q;

	/* Put into a temporary variable first in case `dest` is nullptr. */
	const struct song tmp = q.data[q.head++ % q.size];
	if (dest)
		*dest = tmp;

	return q;
}

struct queue
insert_at(struct queue q, struct song s, int idx)
{
	/* Normalise idx to an unsigned offset from head, clamped to [0, n]. */
	unsigned int n = qsize(q);
	unsigned int pos;
	if (idx >= 0) {
		pos = MIN((unsigned int) idx, n);
	} else {
		/* -1 = last element (same as push), -2 = second-to-last, etc. */
		unsigned int back = (unsigned int) (-idx - 1);
		pos = back >= n ? 0 : n - back;
	}

	/*
	 * Always requeue to normalise head to 0, growing the buffer if
	 * needed.  Afterwards, elements are contiguous at data[0..n-1]
	 * with at least one free slot, so a simple memmove opens a gap.
	 */
	unsigned int newsize = n < q.size ? q.size : MAX(2 * q.size, 1);
	q = requeue(q, newsize);

	memmove(&q.data[pos + 1], &q.data[pos], (n - pos) * sizeof(struct song));
	q.data[pos] = s;
	q.tail++;

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
