#include <errno.h>  /* errno(3p). */
#include <stdio.h>  /* snprintf(3). */
#include <ctype.h>  /* isdigit(3). */
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
	DO(loop);
	DO(consume);
	DO(queue);
	DO(pause);
	DO(play);
	DO(insert);
	DO(clear);
	DO(clearall);
	DO(skip);
	DO(stop);
	DO(toggle);
	DO(volume);
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
cmd_loop(                struct state s,
         [[gnu::unused]] unsigned int argc,
         [[gnu::unused]] const char *args[])
{
	s.mode = LOOP;
	return s;
}

struct state
cmd_consume(                struct state s,
            [[gnu::unused]] unsigned int argc,
            [[gnu::unused]] const char *args[])
{
	s.mode = CONSUME;
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
	if (argc != 1) {
		warn(0, "insert: expected 1 argument, got %u", argc);
		return s;
	}

	char *end;
	long idx = strtol(args[0], &end, 10);
	if (*end != ' ') {
		warn(0, "insert: expected \"<index> <path>\": %s", args[0]);
		return s;
	}

	struct song song;
	if (!parse_song(end + 1, &song)) {
		warn(0, "insert: invalid song: %s", end + 1);
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
cmd_clear(                struct state s,
          [[gnu::unused]] unsigned int argc,
          [[gnu::unused]] const char *args[])
{
	if (qsize(s.queue) <= 1)
		return s;
	for (unsigned int i = s.queue.head + 1; i != s.queue.tail; i++)
		cleanup_song(&s.queue.data[i % s.queue.size]);
	s.queue.tail = s.queue.head + 1;
	return s;
}

struct state
cmd_clearall(                struct state s,
             [[gnu::unused]] unsigned int argc,
             [[gnu::unused]] const char *args[])
{
	while (qsize(s.queue)) {
		[[gnu::cleanup(cleanup_song)]]
		struct song song;
		s.queue = pop(s.queue, &song);
	}
	s.play = STOPPED;
	return s;
}

struct state
cmd_skip(                struct state s,
         [[gnu::unused]] unsigned int argc,
         [[gnu::unused]] const char *args[])
{
	if (qsize(s.queue)) {
		if (s.mode == LOOP) {
			struct song cur;
			s.queue = pop(s.queue, &cur);
			s.queue = push(s.queue, cur);
		} else {
			s.queue = pop(s.queue, nullptr);
		}
	}

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

static bool
parse_volume(const char *arg, unsigned int *out)
{
	unsigned int whole, frac = 0, ndigits = 0;
	const char *p = arg;

	if (*p != '0' && *p != '1')
		return false;
	whole = (unsigned int)(*p++ - '0');

	if (*p == '.') {
		p++;
		while (isdigit((unsigned char)*p)) {
			if (ndigits < 2)
				frac = frac * 10 + (unsigned int)(*p - '0');
			ndigits++;
			p++;
		}
		if (ndigits == 1)
			frac *= 10;
	} else if (*p != '\0') {
		return false;
	}

	if (*p != '\0' || (ndigits == 0 && arg[1] == '.'))
		return false;
	if (whole == 1 && frac != 0)
		return false;

	*out = whole == 1 ? 100 : frac;
	return true;
}

struct state
cmd_volume(struct state s, unsigned int argc, const char *args[])
{
	if (argc != 1) {
		warn(0, "volume: expected 1 argument, got %u", argc);
		return s;
	}

	const char *arg = args[0];
	bool relative = arg[0] == '+' || arg[0] == '-';
	unsigned int parsed;
	if (!parse_volume(relative ? arg + 1 : arg, &parsed)) {
		warn(0, "volume: invalid volume: %s", arg);
		return s;
	}

	if (relative) {
		if (arg[0] == '+') {
			s.volume = s.volume + parsed > 100 ? 100 : s.volume + parsed;
		} else {
			s.volume = parsed > s.volume ? 0 : s.volume - parsed;
		}
	} else {
		s.volume = parsed;
	}

	return s;
}

void
cmd_status(struct state s, int fd)
{
	static const char *const ps[] = {
		[PLAYING] = "playing",
		[PAUSED]  = "paused",
		[STOPPED] = "stopped",
	};
	static const char *const ms[] = {
		[CONSUME] = "consume",
		[EXITING] = "exiting",
		[LOOP]    = "loop",
	};
	char buf[64];
	int n = snprintf(buf, sizeof buf,
	                 "state: %s\nmode: %s\nvolume: %u.%02u\n",
	                 ps[s.play], ms[s.mode], s.volume / 100,
	                 s.volume % 100);
	if (n > 0 && write(fd, buf, (size_t) n) < 0)
		warn(errno, "write");
}

void
cmd_list(struct state s, int fd)
{
	for (unsigned int i = s.queue.head; i != s.queue.tail; i++) {
		const struct song *song = &s.queue.data[i % s.queue.size];
		char uid_line[23]; /* "#" + ULONG_MAX digits + "\n\0" */
		int n = snprintf(uid_line, sizeof uid_line,
		                 "#%lu\n", song->uid);
		if (write(fd, song->path, strlen(song->path)) < 0) {
			warn(errno, "write");
			break;
		}
		if (n > 0 && write(fd, uid_line, (size_t) n) < 0) {
			warn(errno, "write");
			break;
		}
	}
}
