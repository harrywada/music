#include <sys/mman.h> /* shm_open(3), shm_unlink(3). */
#include <fcntl.h> /* O_ constants. */
#include <unistd.h> /* close(2), lseek(2), write(2). */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
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

#tcase ebml_id_eq

#test ebml_id_eq_returns_true_on_match
	uint32_t id = 0x18538067;
	struct vint vint_id = { 4, { 0x18, 0x53, 0x80, 0x67 }};
	ck_assert(ebml_id_eq(id, vint_id));

#test ebml_id_eq_returns_false_on_different
	uint32_t id = 0x18538067;
	struct vint vint_id = { 4, { 0x18, 0x53, 0x81, 0x67 }};
	ck_assert(!ebml_id_eq(id, vint_id));

#test ebml_id_eq_returns_false_on_zero
	uint32_t id = 0;
	struct vint vint_id1 = { 4, { 0x18, 0x53, 0x81, 0x67 }},
	            vint_id2 = { 1, { 1 << 7 }};
	ck_assert(!ebml_id_eq(id, vint_id1));
	ck_assert(!ebml_id_eq(id, vint_id2));

#test ebml_id_eq_returns_false_on_different_size
	uint32_t id = 0x73c5;
	struct vint vint_id = { 3, { 1 << 5, 0x73, 0xc5 }};
	ck_assert(!ebml_id_eq(id, vint_id));

#tcase ebml_descend

#test descend_consumes_input
	uint8_t head[] = {
		0x15, 0x49, 0xa9, 0x66, /* ID. */
		1 << 7 | 1,             /* size. */
	};
	uint8_t data[] = {
		0xff,
	};
	off_t end;
	int fd;

	fd = shm_open("test_descend_consumes_input", O_RDWR|O_CREAT, 0600);
	write(fd, head, sizeof(head));
	write(fd, data, sizeof(data));
	lseek(fd, 0, SEEK_SET);

	end = ebml_descend(fd, 0x1549a966);
	ck_assert_int_eq(end, sizeof(head) + sizeof(data));
	ck_assert_int_eq(lseek(fd, 0, SEEK_CUR), sizeof(head));

	close(fd);
	shm_unlink("test_descend_consumes_input");

#test descend_doesnt_consume_on_unexpected_id
	uint8_t head[] = {
		0x15, 0x49, 0xa9, 0x66, /* ID. */
		1 << 1 | 4,             /* size (too big, shouldn't matter). */
	};
	int fd;

	fd = shm_open("test_descend_doesnt_consume_on_unexpected_id", O_RDWR|O_CREAT, 0600);
	write(fd, head, sizeof(head));
	lseek(fd, 0, SEEK_SET);

	ck_assert_int_eq(ebml_descend(fd, 0x15), -1);
	ck_assert_int_eq(lseek(fd, 0, SEEK_CUR), 0);

	close(fd);
	shm_unlink("test_descend_doesnt_consume_on_unexpected_id");

#test descend_handles_EBML_ANY_ELEMENT
	uint8_t head[] = {
		0x15, 0x49, 0xa9, 0x66, /* ID. */
		1 << 7 | 1,             /* size. */
	};
	uint8_t data[] = {
		0x0f,
	};
	off_t end;
	int fd;

	fd = shm_open("test_descend_handles_EBML_ANY_ELEMENT", O_RDWR|O_CREAT, 0600);
	write(fd, head, sizeof(head));
	write(fd, data, sizeof(data));
	lseek(fd, 0, SEEK_SET);

	end = ebml_descend(fd, EBML_ANY_ELEMENT);
	ck_assert_int_eq(end, sizeof(head) + sizeof(data));
	ck_assert_int_eq(lseek(fd, 0, SEEK_CUR), sizeof(head));

	close(fd);
	shm_unlink("test_descend_handles_EBML_ANY_ELEMENT");

#test descend_handles_invalid_fd
	int fd = -1;
	ck_assert_int_eq(ebml_descend(fd, 15), -1);
