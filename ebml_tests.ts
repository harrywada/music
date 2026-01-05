#include <alloca.h> /* alloca(3). */
#include <fcntl.h> /* O_ constants. */
#include <sys/mman.h> /* shm_open(3), shm_unlink(3). */
#include <unistd.h> /* close(2), lseek(2), write(2). */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "ebml.h"

#define EAL EBML_ANY_ELEMENT /* For brevity. */

int fd; /* Temporary, reusable file descriptor. */

void
fd_setup(void)
{
	fd = shm_open("ebml_tests", O_RDWR|O_CREAT, 0600);
	ck_assert_int_ge(fd, 0);
}

void
fd_teardown(void)
{
	close(fd);
	ck_assert_int_ne(shm_unlink("ebml_tests"), -1);
}


#tcase vint_read

struct {
	size_t size;
	uint8_t data[VINT_OCTET_MAX];
} test_vint_cases[] = {
	{ 1, { 1 << 7 | 1 } },
	{ 3, { 1 << 5, 0, 1 } },
	{ 4, { 0x1a, 0x45, 0xdf, 0xa3 } },
	{ 2, { 0x53, 0x78 } },
};

#test-loop(0, 4) vint_read_reads_from_fd
	struct vint vint;
	unsigned int i;

	uint8_t (* data)[VINT_OCTET_MAX] = &test_vint_cases[_i].data;
	size_t expected_size = test_vint_cases[_i].size;

	write(fd, *data, VINT_OCTET_MAX);
	lseek(fd, 0, SEEK_SET);

	ck_assert(vint_read(fd, &vint));
	ck_assert_int_eq(vint.size, expected_size);
	for (i = 0; i < vint.size; i += 1)
		ck_assert_int_eq(vint.data[i], (*data)[i]);

#tcase vint_value

struct {
	struct vint vint;
	long expect;
} test_value_returns_correct_value_cases[] = {
       /* vint                              expect */
	{ { 1, { 1 << 7 | 5,            }}, 5 },
	{ { 2, { 1 << 6,     5          }}, 5 },
	{ { 5, { 1 << 3,     0, 0, 0, 4 }}, 4 },
};

#test-loop(0, 3) vint_value_returns_correct_value
	struct vint vint = test_value_returns_correct_value_cases[_i].vint;
	long expect = test_value_returns_correct_value_cases[_i].expect;
	ck_assert_int_eq(vint_value(vint), expect);

#test-signal(6) vint_value_aborts_on_too_big
	struct vint vint = { sizeof(long) + 1, { 0 }};
	vint_value(vint);

#tcase ebml_id_eq

struct {
	int expect;
	uint_least32_t id;
	struct vint vint;
} ebml_id_eq_cases[] = {
	{ 1, 0x18538067, { 4, { 0x18, 0x53, 0x80, 0x67 } } },
	{ 1, 0x7373,     { 2, {             0x73, 0x73 } } },
	{ 1, EAL,        { 4, { 0x18, 0x53, 0x80, 0x67 } } },
	{ 1, EAL,        { 1, {                 1 << 7 } } },
	{ 0, 0x18538067, { 4, { 0x18, 0x53, 0x81, 0x67 } } },
	{ 0, 0,          { 4, { 0x18, 0x53, 0x81, 0x67 } } },
	{ 0, 0,          { 1, {                 1 << 7 } } },
	{ 0, 0x73c5,     { 3, { 1 << 5 | 2, 0x73, 0xc5 } } }, /* Extra padding. */
	{ 0, 0x73c5,     { 1, {                   0xc5 } } }, /* Too small. */
};

#test-loop(0, 7) ebml_id_eq_tests
	int expect = ebml_id_eq_cases[_i].expect;
	uint_least32_t id = ebml_id_eq_cases[_i].id;
	struct vint vint = ebml_id_eq_cases[_i].vint;
	ck_assert(!ebml_id_eq(id, vint) == !expect);

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

	write(fd, head, sizeof(head));
	write(fd, data, sizeof(data));
	lseek(fd, 0, SEEK_SET);

	end = ebml_descend(fd, 0x1549a966);
	ck_assert_int_eq(end, sizeof(head) + sizeof(data));
	ck_assert_int_eq(lseek(fd, 0, SEEK_CUR), sizeof(head));

#test descend_doesnt_consume_on_unexpected_id
	uint8_t head[] = {
		0x15, 0x49, 0xa9, 0x66, /* ID. */
		1 << 1 | 4,             /* size (too big, shouldn't matter). */
	};

	write(fd, head, sizeof(head));
	lseek(fd, 0, SEEK_SET);

	ck_assert_int_eq(ebml_descend(fd, 0x15), -1);
	ck_assert_int_eq(lseek(fd, 0, SEEK_CUR), 0);

#test descend_handles_EBML_ANY_ELEMENT
	uint8_t head[] = {
		0x15, 0x49, 0xa9, 0x66, /* ID. */
		1 << 7 | 1,             /* size. */
	};
	uint8_t data[] = {
		0x0f,
	};
	off_t end;

	write(fd, head, sizeof(head));
	write(fd, data, sizeof(data));
	lseek(fd, 0, SEEK_SET);

	end = ebml_descend(fd, EAL);
	ck_assert_int_eq(end, sizeof(head) + sizeof(data));
	ck_assert_int_eq(lseek(fd, 0, SEEK_CUR), sizeof(head));

#tcase ebml_peek

struct {
	uint_least32_t id;
	uint8_t data[VINT_OCTET_MAX];
} peek_returns_id_cases[] = {
	{ 0x5378,     { 0x53, 0x78 } },
	{ 0x1a45dfa3, { 0x1a, 0x45, 0xdf, 0xa3 } },
};

#test-loop(0, 2) peek_returns_id
	uint_least32_t id;
	uint8_t (* data)[VINT_OCTET_MAX];

	id = peek_returns_id_cases[_i].id;
	data = &peek_returns_id_cases[_i].data;

	write(fd, data, VINT_OCTET_MAX);
	lseek(fd, 0, SEEK_SET);

	ck_assert_int_eq(ebml_peek(fd), id);
	ck_assert_int_eq(lseek(fd, 0, SEEK_CUR), 0);

#test peek_handles_overflow_id
	uint8_t bad_id[] = { 0x08, 0xe1, 0x24, 0xd5, 0x69 };

	write(fd, bad_id, sizeof(bad_id));
	lseek(fd, 0, SEEK_SET);

	ck_assert_int_eq(ebml_peek(fd), EAL);
	ck_assert_int_eq(lseek(fd, 0, SEEK_SET), 0);

#tcase ebml_skip

#test skip_consumes_input
	uint8_t head[] = {
		0x73, 0x73, /* ID. */
		1 << 7 | 3, /* size. */
	};
	uint8_t data[] = {
		0xff, 0xff, 0xa9,
	};
	off_t end;

	write(fd, head, sizeof(head));
	write(fd, data, sizeof(data));
	lseek(fd, 0, SEEK_SET);

	end = ebml_skip(fd, 0x7373);
	ck_assert_int_eq(end, sizeof(head) + sizeof(data));
	ck_assert_int_eq(lseek(fd, 0, SEEK_CUR), sizeof(head) + sizeof(data));

#test skip_doesnt_consume_on_unexpected_id
	uint8_t head[] = {
		0x73, 0x73, /* ID. */
		1 << 1 | 1, /* size (too big, shouldn't matter). */
	};

	write(fd, head, sizeof(head));
	lseek(fd, 0, SEEK_SET);

	ck_assert_int_eq(ebml_skip(fd, 0x15), -1);
	ck_assert_int_eq(lseek(fd, 0, SEEK_CUR), 0);

#test skip_handles_EBML_ANY_ELEMENT
	uint8_t head[] = {
		0x73, 0x73, /* ID. */
		1 << 7 | 3, /* size. */
	};
	uint8_t data[] = {
		0xff, 0xff, 0xa9,
	};
	off_t end;

	write(fd, head, sizeof(head));
	write(fd, data, sizeof(data));
	lseek(fd, 0, SEEK_SET);

	end = ebml_skip(fd, EAL);
	ck_assert_int_eq(end, sizeof(head) + sizeof(data));
	ck_assert_int_eq(lseek(fd, 0, SEEK_CUR), sizeof(head) + sizeof(data));

#tcase ebml_readsint

struct {
	uint_least32_t id;
	int num;
	size_t data_sz;
	uint8_t data[sizeof(uint_least32_t) + VINT_OCTET_MAX + sizeof(int64_t)];
} ebml_readsint_cases[] = {
	/* ID      num      data_sz  data[id]    data[size]             data[value] */
	{  0xfb,   3,       3,     { 0xfb,       1 << 7 | 1,                      3 } },
	{  0x75a2, 0x8bd8d, 6,     { 0x75, 0xa2, 1 << 7 | 3,       0x08, 0xbd, 0x8d } },
	{  0x537f, -2,      5,     { 0x53, 0x7f, 1 << 7 | 2,             0xff, 0xfe } },
	{  0x75a2, -594860, 6,     { 0x75, 0xa2, 1 << 7 | 3,       0xf6, 0xec, 0x54 } },
	{  0x537f, 5,       5,     { 0x53, 0x7f, 1 << 7 | 2,                0, 0x05 } },
	{  0x537f, 5,       11,    { 0x53, 0x7f, 1 << 7 | 8, 0, 0, 0, 0, 0, 0, 0, 5 } },
	{  EAL,    12,      4,     { 0xf5,       1 << 7 | 2,                0,   12 } },
};

#test-loop(0, 7) ebml_readsint_tests
	uint_least32_t id = ebml_readsint_cases[_i].id;
	int expect = ebml_readsint_cases[_i].num;
	size_t data_sz = ebml_readsint_cases[_i].data_sz;
	uint8_t *data = ebml_readsint_cases[_i].data;

	int64_t num;

	write(fd, data, data_sz);
	lseek(fd, 0, SEEK_SET);

	ck_assert(ebml_readsint(fd, id, &num));
	ck_assert_int_eq(num, expect);
	ck_assert_int_eq(lseek(fd, 0, SEEK_CUR), data_sz);

#test-signal(6) ebml_readsint_aborts_on_too_big
	uint8_t data[] = { 0x82, 1 << 7 | 9 };
	int64_t num;

	write(fd, data, sizeof(data));
	lseek(fd, 0, SEEK_SET);

	ebml_readsint(fd, 0x82, &num);

#tcase ebml_readuint

struct {
	uint_least32_t id;
	unsigned int num;
	size_t data_sz;
	uint8_t data[sizeof(uint_least32_t) + VINT_OCTET_MAX + sizeof(int64_t)];
} ebml_readuint_cases[] = {
	/* ID      num      data_sz  data[id]    data[size]             data[value] */
	{  0xfb,   3,       3,     { 0xfb,       1 << 7 | 1,                      3 } },
	{  0x75a2, 0x8bd8d, 6,     { 0x75, 0xa2, 1 << 7 | 3,       0x08, 0xbd, 0x8d } },
	{  EAL,    12,      4,     { 0xf5,       1 << 7 | 2,                  0, 12 } },
	{  0x537f, 5,       11,    { 0x53, 0x7f, 1 << 7 | 8, 0, 0, 0, 0, 0, 0, 0, 5 } },
};

#test-loop(0, 4) ebml_readuint_reads_signed_int
	uint_least32_t id = ebml_readuint_cases[_i].id;
	unsigned int expect = ebml_readuint_cases[_i].num;
	size_t data_sz = ebml_readuint_cases[_i].data_sz;
	uint8_t *data = ebml_readuint_cases[_i].data;

	uint64_t num;

	write(fd, data, data_sz);
	lseek(fd, 0, SEEK_SET);

	ck_assert(ebml_readuint(fd, id, &num));
	ck_assert_int_eq(num, expect);
	ck_assert_int_eq(lseek(fd, 0, SEEK_CUR), data_sz);

#test-signal(6) ebml_readuint_aborts_on_too_big
	uint8_t data[] = { 0x82, 1 << 7 | 9 };
	uint64_t num;

	write(fd, data, sizeof(data));
	lseek(fd, 0, SEEK_SET);

	ebml_readuint(fd, 0x82, &num);

#tcase ebml_readfloat

#test ebml_readfloat_reads_float
#test ebml_readfloat_handles_EBML_ANY_ELEMENT
#test ebml_readfloat_aborts_on_size_not_32_64

#tcase ebml_readstring

#test ebml_readstring_reads_string
#test ebml_readstring_handles_EBML_ANY_ELEMENT
#test ebml_readstring_allocates_on_null_pointer

#tcase ebml_readbinary

struct {
	uint_least32_t id;
	size_t data_sz;
	uint8_t data[sizeof(uint_least32_t) + VINT_OCTET_MAX + 8];
} ebml_readbinary_cases[] = {
	{ 0xa1,       5, {                   0xa1, 1 << 7 | 3, 0x01, 0x23, 0x45 } },
	{ 0x1654ae6b, 6, { 0x16, 0x54, 0xae, 0x6b, 1 << 7 | 1,             0xff } },
	{ 0x4dbb,     4, {             0x4d, 0xbb, 1 << 7 | 0,                0 } },
};

#test ebml_readbinary_reads_binary
	uint_least32_t id = ebml_readbinary_cases[_i].id;
	size_t data_sz = ebml_readbinary_cases[_i].data_sz;
	uint8_t *data = ebml_readbinary_cases[_i].data;

	uint8_t buf[8];

	write(fd, data, data_sz);
	lseek(fd, 0, SEEK_SET);

	ck_assert(ebml_readbinary(fd, id, buf, 8));
	/* TODO */
	ck_assert_int_eq(lseek(fd, 0, SEEK_CUR), data_sz);

#main-pre
	tcase_add_checked_fixture(tc1_1, fd_setup, fd_teardown);
	tcase_add_checked_fixture(tc1_4, fd_setup, fd_teardown);
	tcase_add_checked_fixture(tc1_5, fd_setup, fd_teardown);
	tcase_add_checked_fixture(tc1_6, fd_setup, fd_teardown);
	tcase_add_checked_fixture(tc1_7, fd_setup, fd_teardown);
	tcase_add_checked_fixture(tc1_8, fd_setup, fd_teardown);
	tcase_add_checked_fixture(tc1_9, fd_setup, fd_teardown);
	tcase_add_checked_fixture(tc1_10, fd_setup, fd_teardown);
	tcase_add_checked_fixture(tc1_11, fd_setup, fd_teardown);
