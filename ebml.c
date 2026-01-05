#include <errno.h> /* errno(3). */
#include <stddef.h> /* uint8_t(3type). */
#include <stdlib.h> /* abort(3). */
#include <string.h> /* memcmp(3), memcpy(3). */
#include <unistd.h> /* read(2). */

#include <stdio.h> /* fprintf(3). */

#include <sys/types.h>
#include "utils.h" /* pos, seek. */

#include <stddef.h>
#include "utils_le.h" /* rev_octets. */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "ebml.h"

int
vint_read(int fd, struct vint *vint)
{
	uint8_t marker;

	vint->size = 0;

	/* TODO Optimize with __builtin_clgz. */
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

unsigned long
vint_value(struct vint vint)
{
	long result = 0;
	uint8_t mask;
	unsigned int i;

	if (vint.size > sizeof result)
		abort();

	/* TODO Optimize with __builtin_clgz. */
	memcpy(&result, vint.data, vint.size);
	mask = ~(1 << (7 - (vint.size - 1) % 8));
	for (i = 0; i < vint.size; i += 1)
		if (i == (vint.size - 1) / 8)
			((uint8_t *) &result)[i] &= mask;

	rev_octets(&result, vint.size);
	return result;
}

int
ebml_id_eq(uint32_t id, struct vint vint_id)
{
	unsigned int markpos;

	if (id == EBML_ANY_ELEMENT)
		return 1;

	for (markpos = 0; markpos < 32; markpos += 1)
		if (id & (1 << (31 - markpos))) break;
	markpos %= 8; /* EBML IDs are guaranteed to be minimally sized. */

	if (markpos >= 32 || markpos + 1 != vint_id.size)
		return 0;

	rev_octets(&id, vint_id.size);
	return memcmp(&id, vint_id.data, vint_id.size) == 0;
}

off_t
ebml_descend(int fd, uint_least32_t expected_id)
{
	struct vint id, len;
	off_t begin;

	begin = pos(fd);
	if (!vint_read(fd, &id)
	||  !ebml_id_eq(expected_id, id)
	||  !vint_read(fd, &len))
		return seek(fd, begin), -1;

	return pos(fd) + vint_value(len);
}

uint32_t
ebml_peek(int fd)
{
	struct vint id;
	uint_least32_t res;
	off_t begin;

	begin = pos(fd);
	res = EBML_ANY_ELEMENT;
	if (!vint_read(fd, &id) || id.size > sizeof res)
		goto end;

	res = 0; /* Zero-out in case `id.size` is less than `sizeof(result)`. */
	memcpy(&res, id.data, id.size);
	rev_octets(&res, id.size);

end:
	seek(fd, begin);
	return res;
}

off_t
ebml_skip(int fd, uint32_t expected_id)
{
	struct vint id, len;
	off_t begin;

	begin = pos(fd);
	if (!vint_read(fd, &id)
	||  !ebml_id_eq(expected_id, id)
	||  !vint_read(fd, &len))
		return seek(fd, begin), -1;

	return seek(fd, pos(fd) + vint_value(len));
}

int
ebml_readsint(int fd, uint_least32_t expected_id, int64_t *dest)
{
	struct vint id, l;
	unsigned long len;
	off_t begin;

	begin = pos(fd);
	if (!(   vint_read(fd, &id)
	      && ebml_id_eq(expected_id, id)
	      && vint_read(fd, &l))
	||  (len = vint_value(l)) > sizeof *dest
	||  read(fd, dest, len) == -1)
		return seek(fd, begin), 0;

	rev_octets(dest, len);
	switch (len) {
	case 1: *dest = *(int8_t  *) dest; break;
	case 2: *dest = *(int16_t *) dest; break;
	case 4: *dest = *(int32_t *) dest; break;
	case 8:                            break;
	default:
		{
			uint64_t mask = UINT64_MAX << (7 + 8 * (len - 1));
			if (*dest & (1 << (7 + 8 * (len - 1)))) {
				*dest |= mask;
			} else    *dest &= ~mask;
		}
		break;
	}

	return 1;
}

int
ebml_readuint(int fd, uint_least32_t expected_id, uint64_t *dest)
{
	struct vint id, l;
	unsigned long len;
	off_t begin;

	memset(dest, 0, sizeof *dest);
	begin = pos(fd);
	if (!(   vint_read(fd, &id)
	      && ebml_id_eq(expected_id, id)
	      && vint_read(fd, &l))
	|| (len = vint_value(l)) > sizeof *dest
	|| read(fd, dest, len) == -1)
		return seek(fd, begin), 0;

	rev_octets(dest, len);
	return 1;
}

int
ebml_readfloat(int fd, uint_least32_t expected_id, double *dest)
{
	struct vint id, len;
	float f;
	off_t begin;

	begin = pos(fd);
	if (!vint_read(fd, &id)
	||  !ebml_id_eq(expected_id, id)
	||  !vint_read(fd, &len))
		return seek(fd, begin), 0;

	switch (vint_value(len)) {
	case 8:
		if (read(fd, dest, 8) == -1)
			goto err;
		rev_octets(dest, sizeof *dest);
		break;
	case 4:
		if (read(fd, &f, 4) == -1)
			goto err;
		rev_octets(&f, sizeof f);
		*dest = f;
		break;
	case 0:
		/* TODO. */
		abort();
		break;
	default:
		abort();
	}

	return 1;

err:
	seek(fd, begin);
	return 0;
}

int
ebml_readstring(int fd, uint_least32_t expected_id, char **dest, size_t *sz)
{
	struct vint id, len;
	off_t begin;

	begin = pos(fd);
	if (!vint_read(fd, &id)
	||  !ebml_id_eq(expected_id, id)
	||  !vint_read(fd, &len))
		return seek(fd, begin), 0;

	if (*sz < vint_value(len)) {
		*sz  = vint_value(len);
		*dest = realloc(*dest, vint_value(len));
	}

	if (read(fd, *dest, vint_value(len)) == -1)
		return seek(fd, begin), 0;

	return 1;
}

/*
int ebml_readdate(int fd, uint_least32_t expected_id, time_t *dest)
{

}
*/

int
ebml_readbinary(int fd, uint_least32_t expected_id, void *dest, size_t dest_sz)
{
	struct vint id, l;
	unsigned long len;
	off_t begin;

	memset(dest, 0, dest_sz);
	begin = pos(fd);
	if (!(   vint_read(fd, &id)
	      && ebml_id_eq(expected_id, id)
	      && vint_read(fd, &l))
	||  (len = vint_value(l)) > dest_sz
	||  read(fd, dest, len) == -1)
		return seek(fd, begin), 0;

	rev_octets(dest, len);
	return 1;
}
