#include <sys/mman.h> /* shm_open(3), shm_unlink(3). */
#include <fcntl.h> /* O_ constants. */
#include <unistd.h> /* close(2), lseek(2), write(2). */

#include <stddef.h>
#include <stdint.h>
#include "ebml.h"

#tcase vint_read

struct {
	size_t size;
	uint8_t data[VINT_OCTET_MAX];
} test_vint_cases[] = {
	{ 1, { 1 << 7 | 1 }},
	{ 3, { 1 << 5, 0, 1 }},
	{ 4, { 0x1a, 0x45, 0xdf, 0xa3 }},
};

#test-loop(0, 3) vint_read_reads_from_fd
	struct vint vint;
	int fd;
	unsigned int i;

	uint8_t (* data)[VINT_OCTET_MAX] = &test_vint_cases[_i].data;
	size_t expected_size = test_vint_cases[_i].size;

	fd = shm_open("test_vint_reads_from_fd", O_RDWR|O_CREAT, 0600);
	write(fd, *data, VINT_OCTET_MAX);
	lseek(fd, 0, SEEK_SET);

	ck_assert(vint_read(fd, &vint));
	ck_assert_int_eq(vint.size, expected_size);
	for (i = 0; i < vint.size; i += 1)
		ck_assert_int_eq(vint.data[i], (*data)[i]);

	close(fd);
	shm_unlink("test_vint_reads_from_fd");

#test vint_read_returns_false_on_error
	struct vint vint;
	int fd = -1;
	ck_assert(!vint_read(fd, &vint));

#tcase vint_value

#test vint_value_returns_correct_value
	struct vint vint = { 1, { 1 << 7 | 5 }};
	ck_assert_int_eq(vint_value(vint), 5);

#test-signal(6) vint_value_aborts_on_too_big
	struct vint vint = { sizeof(long) + 1, { 0 }};
	vint_value(vint);
