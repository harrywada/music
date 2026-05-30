#include <stdarg.h> /* va_list(3type), va_start(3), va_end(3). */
#include <stdlib.h> /* abort(3), exit(3). */
#include <syslog.h> /* LOG_*(3). */
#include <unistd.h> /* lseek(2), off_t(3type). */

#include "utils.h"

void log_msg(int, int, char *, va_list);

void
debug(int err, char *msg, ...)
{
	va_list args;
	va_start(args, msg);
	log_msg(LOG_DEBUG, err, msg, args);
	va_end(args);
}

void
warn(int err, char *msg, ...)
{
	va_list args;
	va_start(args, msg);
	log_msg(LOG_WARNING, err, msg, args);
	va_end(args);
}

void
die(int err, char *msg, ...)
{
	va_list args;
	va_start(args, msg);
	log_msg(LOG_ERR, err, msg, args);
	va_end(args);
	exit(EXIT_FAILURE);
}

off_t
pos(int fd)
{
	off_t result;

	if ((result = lseek(fd, 0, SEEK_CUR)) == (off_t) -1)
		abort();
	return result;
}

off_t
seek(int fd, off_t offset)
{
	off_t result;

	if ((result = lseek(fd, offset, SEEK_SET)) == (off_t) -1)
		abort();
	return result;
}
