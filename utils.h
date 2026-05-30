#pragma once

#include <sys/types.h>

#define MAX(a, b) ((a) < (b) ? (b) : (a))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

/*
#define SND(fn, ...) \
if ((err = snd_pcm_ ## fn (__VA_ARGS__)) < 0)
	error(EXIT_FAILURE, 0, "snd_pcm_" #fn ": %s", snd_strerror(err))
*/

[[gnu::format(gnu_printf, 2, 3)]]
void debug(int, char *, ...);
[[gnu::format(gnu_printf, 2, 3)]]
void warn(int, char *, ...);
[[noreturn, gnu::format(gnu_printf, 2, 3)]]
void die(int, char *, ...);

off_t pos(int);
off_t seek(int, off_t);
