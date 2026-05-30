#include <errno.h> /* errno(3). */
#include <stddef.h> /* uint8_t(3type). */
#include <stdlib.h> /* abort(3). */
#include <string.h> /* memcpy(3), memset(3). */
#include <unistd.h> /* read(2). */

#include <stdio.h> /* fprintf(3). */

#include <sys/types.h>
#include "utils.h" /* pos, seek. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "ebml.h"

[[gnu::fd_arg_read(1)]]
static int
read_be(int fd, uint64_t *out, size_t n)
{
	uint8_t buf[sizeof(uint64_t)];
	uint64_t val = 0;

	if (n > sizeof buf || read(fd, buf, n) != (ssize_t)n)
		return 0;
	for (size_t i = 0; i < n; i++)
		val = (val << 8) | buf[i];
	*out = val;
	return 1;
}

int
vint_read(int fd, struct vint *vint)
{
	uint8_t first;
	int extra;

	if (read(fd, &first, 1) != 1) {
		errno = 0;
		return 0;
	}

	if (!first)
		return 0;

#if __has_builtin(__builtin_clz)
	extra = __builtin_clz((unsigned)first << (sizeof(unsigned) * 8u - 8u));
#else
	extra = 0;
	for (uint8_t b = first; !(b & 0x80); b <<= 1)
		extra++;
#endif

	if (extra >= VINT_OCTET_MAX)
		return 0;

	vint->size = extra + 1;
	vint->raw  = first;

	if (extra > 0) {
		uint8_t buf[VINT_OCTET_MAX - 1];
		if (read(fd, buf, extra) != extra) {
			errno = 0;
			return 0;
		}
		for (int i = 0; i < extra; i++)
			vint->raw = (vint->raw << 8) | buf[i];
	}

	return 1;
}

uint64_t
vint_value(struct vint vint)
{
#if __has_builtin(__builtin_unreachable)
	if (vint.size > VINT_OCTET_MAX)
		__builtin_unreachable();
#endif
	return vint.raw & ~(UINT64_C(1) << (7 * vint.size));
}

int
ebml_id_eq(uint32_t id, struct vint vint_id)
{
	unsigned int markpos;

	if (id == EBML_ANY_ELEMENT)
		return 1;

	for (markpos = 0; markpos < 32; markpos += 1)
		if (id & (UINT32_C(1) << (31 - markpos))) break;
	markpos %= 8; /* EBML IDs are guaranteed to be minimally sized. */

	if (markpos + 1 != vint_id.size)
		return 0;

	return (uint32_t) vint_id.raw == id;
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
	res = 0;
	if (!vint_read(fd, &id) || id.size > sizeof res)
		goto end;

	res = (uint_least32_t) id.raw;

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
	uint64_t len, val;
	off_t begin;

	begin = pos(fd);
	if (!(   vint_read(fd, &id)
	      && ebml_id_eq(expected_id, id)
	      && vint_read(fd, &l))
	||  (len = vint_value(l)) > sizeof *dest
	||  !read_be(fd, &val, len))
		return seek(fd, begin), 0;

	switch (len) {
	case 0: *dest = 0;             break;
	case 1: *dest = (int8_t)  val; break;
	case 2: *dest = (int16_t) val; break;
	case 4: *dest = (int32_t) val; break;
	case 8: *dest = (int64_t) val; break;
	default:
		if ((val >> (8 * len - 1)) & 1)
			*dest = (int64_t)(val | (UINT64_MAX << (8 * len)));
		else
			*dest = (int64_t) val;
		break;
	}

	return 1;
}

[[gnu::fd_arg_read(1)]]
static int
ebml_readuint_raw(int fd, uint_least32_t expected_id, off_t *begin, uint64_t *val)
{
	struct vint id, l;
	uint64_t len;

	*begin = pos(fd);
	if (!(   vint_read(fd, &id)
	      && ebml_id_eq(expected_id, id)
	      && vint_read(fd, &l))
	|| (len = vint_value(l)) > sizeof(uint64_t)
	|| !read_be(fd, val, len))
		return seek(fd, *begin), 0;

	return 1;
}

int
_ebml_readuint_u64(int fd, uint_least32_t id, uint64_t *dest)
{
	off_t begin;
	return ebml_readuint_raw(fd, id, &begin, dest);
}

int
_ebml_readuint_u32(int fd, uint_least32_t id, uint32_t *dest)
{
	off_t begin;
	uint64_t val;

	if (!ebml_readuint_raw(fd, id, &begin, &val) || val > UINT32_MAX)
		return seek(fd, begin), 0;
	*dest = (uint32_t) val;
	return 1;
}

int
_ebml_readuint_u8(int fd, uint_least32_t id, uint8_t *dest)
{
	off_t begin;
	uint64_t val;

	if (!ebml_readuint_raw(fd, id, &begin, &val) || val > UINT8_MAX)
		return seek(fd, begin), 0;
	*dest = (uint8_t) val;
	return 1;
}

int
_ebml_readuint_bool(int fd, uint_least32_t id, bool *dest)
{
	off_t begin;
	uint64_t val;

	if (!ebml_readuint_raw(fd, id, &begin, &val) || val > 1)
		return seek(fd, begin), 0;
	*dest = (bool) val;
	return 1;
}

int
ebml_readfloat(int fd, uint_least32_t expected_id, double *dest)
{
	struct vint id, len;
	off_t begin;

	begin = pos(fd);
	if (!vint_read(fd, &id)
	||  !ebml_id_eq(expected_id, id)
	||  !vint_read(fd, &len))
		return seek(fd, begin), 0;

	switch (vint_value(len)) {
	case 8: {
		uint64_t tmp;
		if (!read_be(fd, &tmp, 8))
			goto err;
		memcpy(dest, &tmp, 8);
		break;
	}
	case 4: {
		uint64_t tmp;
		uint32_t tmp32;
		float f;
		if (!read_be(fd, &tmp, 4))
			goto err;
		tmp32 = (uint32_t) tmp;
		memcpy(&f, &tmp32, sizeof f);
		*dest = f;
		break;
	}
	case 0:
		*dest = 0.0;
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
		*dest = realloc(*dest, vint_value(len));
		if (*dest == nullptr)
			return seek(fd, begin), 0;
	}
	*sz = vint_value(len);

	if (read(fd, *dest, vint_value(len)) == -1)
		return seek(fd, begin), 0;

	return 1;
}

int
ebml_readdate(int fd, uint_least32_t expected_id, time_t *dest)
{
	int64_t val;
	if (!ebml_readsint(fd, expected_id, &val))
		return 0;
	*dest = (time_t)(val / 1000000000 + 978307200);
	return 1;
}

int
ebml_readbinary(int fd, uint_least32_t expected_id, void *dest, size_t dest_sz)
{
	struct vint id, l;
	uint64_t len;
	off_t begin;

	memset(dest, 0, dest_sz);
	begin = pos(fd);
	if (!(   vint_read(fd, &id)
	      && ebml_id_eq(expected_id, id)
	      && vint_read(fd, &l))
	||  (len = vint_value(l)) > dest_sz
	||  read(fd, dest, len) == -1)
		return seek(fd, begin), 0;

	return 1;
}
