#include <errno.h> /* errno(3). */
#include <stddef.h> /* uint8_t(3type). */
#include <stdlib.h> /* abort(3). */
#include <string.h> /* memcmp(3), memcpy(3). */
#include <unistd.h> /* read(2). */

#include <sys/types.h>
#include "utils.h" /* pos, seek. */

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#include <stddef.h>
#include "utils_le.h" /* rev_octets. */
#endif

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
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

int
ebml_id_eq(uint32_t id, struct vint vint_id)
{
	unsigned int markpos;

	for (markpos = 0; markpos < 32; markpos += 1)
		if (id & (1 << (31 - markpos))) break;
	markpos %= 8; /* EBML IDs are guaranteed to be minimally sized. */

	if (markpos >= 32 || markpos + 1 != vint_id.size)
		return 0;

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	rev_octets(&id, vint_id.size);
#elif __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__
	abort();
#endif

	return memcmp(&id, vint_id.data, vint_id.size) == 0;
}

off_t
ebml_descend(int fd, uint32_t expected_id)
{
	struct vint id, len;
	off_t begin;

	if (fd < 0)
		return -1;

	begin = pos(fd);
	if (!vint_read(fd, &id)
	||  (expected_id != EBML_ANY_ELEMENT && !ebml_id_eq(expected_id, id))
	||  !vint_read(fd, &len)) {
		seek(fd, begin);
		return -1;
	}

	return pos(fd) + vint_value(len);
}
