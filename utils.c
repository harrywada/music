#include <stdarg.h> /* va_list(3type), va_start(3). */
#include <stdio.h> /* printf(3). */
#include <stdlib.h> /* abort(3), exit(3). */
#include <string.h> /* strerror(3). */
#include <unistd.h> /* lseek(2), off_t(3type). */

#include <sys/types.h>
#include "utils.h"

void
error(int stat, int err, char *msg, ...)
{
	va_list args;

	va_start(args, msg);
	vfprintf(stderr, msg, args);

	if (err)
		fprintf(stderr, ": %s", strerror(err));
	fprintf(stderr, "\n");

	if (stat) exit(stat);
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
