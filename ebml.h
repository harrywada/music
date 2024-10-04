/* Include: stdint.h, stddef.h, sys/types.h. */

#define EBML_ANY_ELEMENT UINT32_MAX /* Illegal EBML ID. */

struct vint {
	size_t size;
	uint8_t data[VINT_OCTET_MAX]; /* Includes `VINT_MARKER`. */
};

int vint_read(int fd, struct vint *);
long vint_value(struct vint);

int ebml_id_eq(uint32_t, struct vint); /* Assume IDs are at most four bytes. */

off_t ebml_descend(int, uint32_t); /* Assume IDs are at most four bytes. */
off_t ebml_skip(int, uint32_t); /* Assume IDs are at most four bytes. */
