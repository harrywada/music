#include <stdint.h> /* uint8_t(3type). */

#include <stddef.h>
#include "utils_le.h"

void
rev_octets(void *ptr, size_t n)
{
	void *end = (uint8_t *) ptr + n - 1;
	uint8_t buf;

	while (ptr < end) {
		buf = *(uint8_t *) ptr;
		*(uint8_t *) ptr++ = *(uint8_t *) end;
		*(uint8_t *) end-- = buf;
	}
}
