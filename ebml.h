#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#define EBML_HEADER 0x1a45dfa3
#define EBML_ANY_ELEMENT UINT32_MAX /* Illegal EBML ID. */

struct vint {
	size_t size;
	uint8_t data[VINT_OCTET_MAX]; /* Includes `VINT_MARKER`. */
};

[[gnu::fd_arg_read(1)]]
int vint_read(int fd, struct vint *);
unsigned long vint_value(struct vint);

[[gnu::const]]
int ebml_id_eq(uint_least32_t, struct vint); /* Assume IDs are at most four bytes. */

[[gnu::fd_arg_read(1)]]
off_t ebml_descend(int, uint_least32_t); /* Assume IDs are at most four bytes. */
[[gnu::fd_arg_read(1)]]
uint32_t ebml_peek(int); /* Assume IDs are at most four bytes. */
[[gnu::fd_arg_read(1)]]
off_t ebml_skip(int, uint_least32_t); /* Assume IDs are at most four bytes. */

[[gnu::fd_arg_read(1)]]
int ebml_readsint(int, uint_least32_t, int64_t *);
[[gnu::fd_arg_read(1)]]
int ebml_readuint(int, uint_least32_t, uint64_t *);
[[gnu::fd_arg_read(1)]]
int ebml_readfloat(int, uint_least32_t, double *);
[[gnu::fd_arg_read(1)]]
int ebml_readstring(int, uint_least32_t, char **, size_t *); /* Handles UTF-8 and ASCII. */
[[gnu::fd_arg_read(1)]]
int ebml_readdate(int, uint_least32_t, time_t *);
[[gnu::fd_arg_read(1)]]
int ebml_readbinary(int, uint_least32_t, void *, size_t);
