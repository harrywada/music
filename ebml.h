/* Include: stdint.h, stddef.h. */

struct vint {
	size_t size;
	uint8_t data[VINT_OCTET_MAX]; /* Includes `VINT_MARKER`. */
};

int vint_read(int fd, struct vint *);
long vint_value(struct vint);
