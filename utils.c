#include <stdlib.h> /* abort(3). */
#include <unistd.h> /* lseek(2), off_t(3type). */

#include <sys/types.h>
#include "utils.h"

off_t
pos(int fd)
{
	off_t result;

	if ((result = lseek(fd, 0, SEEK_CUR)) == (off_t) -1)
		abort();
	return result;
}
