#include <stdio.h>  /* snprintf(3). */
#include <stdlib.h> /* strtol(3). */
#include <string.h> /* strcmp(3), strlen(3). */
#include <unistd.h> /* write(2). */

#include "cmds.h"
#include "song.h"
#include "state.h"
#include "utils.h"

struct state
cmd(struct state s, unsigned int argc, const char *args[])
{
#define DO(cmd)                         \
	if (strcmp(args[0], #cmd) == 0) \
		return cmd_ ## cmd(s, argc - 1, &args[1])
	DO(exit);
	DO(queue);
	DO(pause);
	DO(play);
	DO(insert);
	DO(skip);
	DO(stop);
	DO(toggle);
#undef DO

	warn(0, "unknown command: %s", args[0]);
	return s;
}

struct state
cmd_exit(                struct state s,
         [[gnu::unused]] unsigned int argc,
         [[gnu::unused]] const char *args[])
{
	s.mode = EXITING;
	return s;
}

struct state
cmd_queue(struct state s, unsigned int argc, const char *args[])
{
	if (argc != 1) {
		warn(0, "queue: expected 1 argument, got %u", argc);
		return s;
	}

	struct song song;
	if (!parse_song(args[0], &song)) {
		warn(0, "queue: invalid song: %s", args[0]);
		return s;
	}

	auto newq = push(s.queue, song);
	if (qsize(newq) != qsize(s.queue) + 1) {
		warn(0, "queue: failed to push song");
		return s;
	}

	s.queue = newq;
	return s;
}

struct state
cmd_pause(                struct state s,
          [[gnu::unused]] unsigned int argc,
          [[gnu::unused]] const char *args[])
{
	s.play = PAUSED;
	return s;
}

struct state
cmd_play(                struct state s,
         [[gnu::unused]] unsigned int argc,
         [[gnu::unused]] const char *args[])
{
	s.play = PLAYING;
	return s;
}

struct state
cmd_insert(struct state s, unsigned int argc, const char *args[])
{
	if (argc != 2) {
		warn(0, "insert: expected 2 arguments, got %u", argc);
		return s;
	}

	struct song song;
	if (!parse_song(args[0], &song)) {
		warn(0, "insert: invalid song: %s", args[0]);
		return s;
	}

	char *end;
	long idx = strtol(args[1], &end, 10);
	if (*end != '\0') {
		warn(0, "insert: invalid index: %s", args[1]);
		cleanup_song(&song);
		return s;
	}

	auto newq = insert_at(s.queue, song, (int) idx);
	if (qsize(newq) != qsize(s.queue) + 1) {
		warn(0, "insert: failed to insert song");
		return s;
	}

	s.queue = newq;
	return s;
}

struct state
cmd_skip(                struct state s,
         [[gnu::unused]] unsigned int argc,
         [[gnu::unused]] const char *args[])
{
	if (qsize(s.queue))
		s.queue = pop(s.queue, nullptr);

	if (!qsize(s.queue))
		s.play = STOPPED;

	return s;
}

struct state
cmd_stop(                struct state s,
         [[gnu::unused]] unsigned int argc,
         [[gnu::unused]] const char *args[])
{
	s.play = STOPPED;
	return s;
}

struct state
cmd_toggle(                struct state s,
           [[gnu::unused]] unsigned int argc,
           [[gnu::unused]] const char *args[])
{
	if (s.play == PLAYING)
		s.play = PAUSED;
	else if (s.play == PAUSED)
		s.play = PLAYING;
	return s;
}

void
cmd_list(struct state s, int fd)
{
	for (unsigned int i = s.queue.head; i != s.queue.tail; i++) {
		const struct song *song = &s.queue.data[i % s.queue.size];
		char uid_line[23]; /* "#" + ULONG_MAX digits + "\n\0" */
		int n = snprintf(uid_line, sizeof uid_line,
		                 "#%lu\n", song->uid);
		(void) write(fd, song->path, strlen(song->path));
		if (n > 0)
			(void) write(fd, uid_line, (size_t) n);
	}
}
