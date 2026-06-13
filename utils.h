#pragma once

#include <sys/types.h>

#define MAX(a, b) ((a) < (b) ? (b) : (a))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

[[gnu::format(gnu_printf, 2, 3)]]
void debug(int, char *, ...);
[[gnu::format(gnu_printf, 2, 3)]]
void warn(int, char *, ...);
[[noreturn, gnu::format(gnu_printf, 2, 3)]]
void die(int, char *, ...);

off_t pos(int);
off_t seek(int, off_t);
