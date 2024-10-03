#include <errno.h> /* errno(3). */
#include <stddef.h> /* uint8_t(3type). */
#include <stdlib.h> /* abort(3). */
#include <string.h> /* memcpy(3). */
#include <unistd.h> /* read(2). */

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#include <stddef.h>
#include "utils_le.h" /* rev_octets. */
#endif

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

long
vint_value(struct vint vint)
{
	long result = 0;
	uint8_t mask;
	unsigned int i;

	if (vint.size > sizeof(result))
		abort();

	memcpy(&result, vint.data, vint.size);
#if __BYTE_ORDER__ == __LITTLE_ENDIAN__
	rev_octets(&result, vint.size);
#endif
	mask = ~(1 << (7 - (vint.size - 1) % 8));
	for (i = 0; i < vint.size; i += 1)
		if (i == (vint.size - 1) / 8)
			((uint8_t *) &result)[i] &= mask;

	return result;
}
