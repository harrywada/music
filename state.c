#include "queue.h"
#include "state.h"

bool
mkstate(struct state *s)
{
	if (!mkqueue(&s->queue, 64))
		return false;
	s->mode = CONSUME;
	s->cur = s->queue.head;
	s->play = STOPPED;

	return true;
}

void
cleanup_state(struct state *s)
{
	cleanup_queue(&s->queue);
}
