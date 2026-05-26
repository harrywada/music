#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#define EBML_HEADER 0x1a45dfa3
#define EBML_ANY_ELEMENT UINT32_MAX /* Illegal EBML ID. */

struct vint {
	uint8_t  size;
	uint64_t raw; /* Full encoded value, VINT_MARKER included. */
};

[[gnu::fd_arg_read(1), gnu::nonnull(2)]]
int vint_read(int fd, struct vint *);
[[gnu::const]]
uint64_t vint_value(struct vint);

[[gnu::const]]
int ebml_id_eq(uint_least32_t, struct vint); /* Assume IDs are at most four bytes. */

[[gnu::fd_arg_read(1)]]
off_t ebml_descend(int, uint_least32_t); /* Assume IDs are at most four bytes. */
[[gnu::fd_arg_read(1)]]
uint32_t ebml_peek(int); /* Returns 0 on error. IDs are at most four bytes. */
[[gnu::fd_arg_read(1)]]
off_t ebml_skip(int, uint_least32_t); /* Assume IDs are at most four bytes. */

[[gnu::fd_arg_read(1), gnu::nonnull(3)]]
int ebml_readsint(int, uint_least32_t, int64_t *);
[[gnu::fd_arg_read(1), gnu::nonnull(3)]]
int _ebml_readuint_u64 (int, uint_least32_t, uint64_t *);
[[gnu::fd_arg_read(1), gnu::nonnull(3)]]
int _ebml_readuint_u32 (int, uint_least32_t, uint32_t *);
[[gnu::fd_arg_read(1), gnu::nonnull(3)]]
int _ebml_readuint_u8  (int, uint_least32_t, uint8_t *);
[[gnu::fd_arg_read(1), gnu::nonnull(3)]]
int _ebml_readuint_bool(int, uint_least32_t, bool *);
#define ebml_readuint(fd, id, dest) _Generic(*(dest),  \
    uint64_t: _ebml_readuint_u64,                      \
    uint32_t: _ebml_readuint_u32,                      \
    uint8_t:  _ebml_readuint_u8,                       \
    bool:     _ebml_readuint_bool                      \
)(fd, id, dest)
[[gnu::fd_arg_read(1), gnu::nonnull(3)]]
int ebml_readfloat(int, uint_least32_t, double *);
[[gnu::fd_arg_read(1), gnu::nonnull(3, 4)]]
int ebml_readstring(int, uint_least32_t, char **, size_t *); /* Handles UTF-8 and ASCII. */
[[gnu::fd_arg_read(1), gnu::nonnull(3)]]
int ebml_readdate(int, uint_least32_t, time_t *);
[[gnu::fd_arg_read(1), gnu::nonnull(3)]]
int ebml_readbinary(int, uint_least32_t, void *, size_t);
