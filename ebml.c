#include <errno.h> /* errno(3). */
#include <stddef.h> /* uint8_t(3type). */
#include <unistd.h> /* read(2). */

#include <stddef.h>
#include <stdint.h>
#include "ebml.h"

int
vint_read(int fd, struct vint *vint)
{
	uint8_t marker;

	vint->size = 0;

	do {
		if (vint->size + 1 > VINT_OCTET_MAX) {
			return 0;
		}
		if (read(fd, &vint->data[vint->size++], sizeof(uint8_t)) == -1) {
			errno = 0;
			return 0;
		}

		marker = 1 << (7 - (vint->size - 1) % 8);
	} while (!(vint->data[(vint->size - 1) / 8] & marker));

	return 1;
}
