#include <alloca.h> /* alloca(3). */
#include <fcntl.h> /* O_ constants. */
#include <stdlib.h> /* free(3), malloc(3). */
#include <sys/mman.h> /* shm_open(3), shm_unlink(3). */
#include <unistd.h> /* close(2), lseek(2), write(2). */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "ebml.h"

#suite ebml

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
	uint8_t  size;
	uint64_t raw;
	uint8_t  bytes[VINT_OCTET_MAX]; /* Wire bytes written to fd. */
} test_vint_cases[] = {
	{ 1, 0x81,       { 0x81 } },
	{ 3, 0x200001,   { 0x20, 0x00, 0x01 } },
	{ 4, 0x1a45dfa3, { 0x1a, 0x45, 0xdf, 0xa3 } },
	{ 2, 0x5378,     { 0x53, 0x78 } },
};

#test-loop(0, 4) vint_read_reads_from_fd
	struct vint vint;

	uint8_t (* bytes)[VINT_OCTET_MAX] = &test_vint_cases[_i].bytes;
	uint8_t  expected_size            = test_vint_cases[_i].size;
	uint64_t expected_raw             = test_vint_cases[_i].raw;

	write(fd, *bytes, VINT_OCTET_MAX);
	lseek(fd, 0, SEEK_SET);

	ck_assert(vint_read(fd, &vint));
	ck_assert_int_eq(vint.size, expected_size);
	ck_assert_int_eq(vint.raw, expected_raw);

#test vint_read_fails_on_eof
	/* 0x40 = 0100 0000: marker at bit 6 means a 2-byte VINT, but the fd
	   only has this one byte, so the second read hits EOF. */
	uint8_t data[] = { 0x40 };
	struct vint vint;

	write(fd, data, sizeof(data));
	lseek(fd, 0, SEEK_SET);

	ck_assert(!vint_read(fd, &vint));

#tcase vint_value

struct {
	struct vint vint;
	long expect;
} test_value_returns_correct_value_cases[] = {
       /* vint                            expect */
	{ { 1, 0x85                    }, 5 },
	{ { 2, 0x4005                  }, 5 },
	{ { 5, UINT64_C(0x0800000004)  }, 4 },
};

#test-loop(0, 3) vint_value_returns_correct_value
	struct vint vint = test_value_returns_correct_value_cases[_i].vint;
	long expect = test_value_returns_correct_value_cases[_i].expect;
	ck_assert_int_eq(vint_value(vint), expect);

#test-signal(6) vint_value_aborts_on_too_big
	struct vint vint = { sizeof(long) + 1, 0 };
	vint_value(vint);

#tcase ebml_id_eq

struct {
	int expect;
	uint_least32_t id;
	struct vint vint;
} ebml_id_eq_cases[] = {
	{ 1, 0x18538067, { 4, 0x18538067 } },
	{ 1, 0x7373,     { 2, 0x7373     } },
	{ 1, EAL,        { 4, 0x18538067 } },
	{ 1, EAL,        { 1, 0x80       } },
	{ 0, 0x18538067, { 4, 0x18538167 } },
	{ 0, 0,          { 4, 0x18538167 } },
	{ 0, 0,          { 1, 0x80       } },
	{ 0, 0x73c5,     { 3, 0x2273c5   } }, /* Extra padding. */
	{ 0, 0x73c5,     { 1, 0xc5       } }, /* Too small. */
};

#test-loop(0, 9) ebml_id_eq_tests
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
	/* ID      num          data_sz  data[id]    data[size]             data[value] */
	{  0xfb,   3,           3,     { 0xfb,       1 << 7 | 1,                      3 } },
	{  0x75a2, 0x8bd8d,     6,     { 0x75, 0xa2, 1 << 7 | 3,       0x08, 0xbd, 0x8d } },
	{  0x537f, -2,          5,     { 0x53, 0x7f, 1 << 7 | 2,             0xff, 0xfe } },
	{  0x75a2, -594860,     6,     { 0x75, 0xa2, 1 << 7 | 3,       0xf6, 0xec, 0x54 } },
	{  0x537f, 5,           5,     { 0x53, 0x7f, 1 << 7 | 2,                0, 0x05 } },
	{  0x537f, 5,           11,    { 0x53, 0x7f, 1 << 7 | 8, 0, 0, 0, 0, 0, 0, 0, 5 } },
	{  EAL,    12,          4,     { 0xf5,       1 << 7 | 2,                0,   12 } },
	/* 5-byte: exercises the default sign-extension branch. */
	{  0x537f, 5,           8,     { 0x53, 0x7f, 1 << 7 | 5, 0x00, 0x00, 0x00, 0x00, 0x05 } },
	/* 5-byte negative: bit 39 set, bit 7 clear — catches the 1<<39 UB. */
	{  0x537f, -256,        8,     { 0x53, 0x7f, 1 << 7 | 5, 0xff, 0xff, 0xff, 0xff, 0x00 } },
	/* 7-byte negative: bit 55 set, bit 23 clear — catches the 1<<55 UB. */
	{  0x537f, -33554432,  10,     { 0x53, 0x7f, 1 << 7 | 7,
	                                 0xff, 0xff, 0xff, 0xfe, 0x00, 0x00, 0x00 } },
};

#test-loop(0, 10) ebml_readsint_tests
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

#test ebml_readsint_returns_zero_for_zero_length
	/* Zero-length signed integer has an implied value of 0. */
	uint8_t data[] = { 0xfb, 1 << 7 | 0 };
	int64_t num = INT64_MAX; /* sentinel — must be overwritten */

	write(fd, data, sizeof(data));
	lseek(fd, 0, SEEK_SET);

	ck_assert(ebml_readsint(fd, 0xfb, &num));
	ck_assert_int_eq(num, 0);
	ck_assert_int_eq(lseek(fd, 0, SEEK_CUR), sizeof(data));

#test ebml_readsint_returns_false_on_id_mismatch
	uint8_t data[] = { 0xfb, 1 << 7 | 1, 3 };
	int64_t num;

	write(fd, data, sizeof(data));
	lseek(fd, 0, SEEK_SET);

	ck_assert(!ebml_readsint(fd, 0x82, &num));
	ck_assert_int_eq(lseek(fd, 0, SEEK_CUR), 0);

#test ebml_readsint_returns_false_when_value_too_large
	/* Value length 9 exceeds sizeof(int64_t) = 8; must return false. */
	uint8_t data[] = { 0x82, 1 << 7 | 9, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	int64_t num;

	write(fd, data, sizeof(data));
	lseek(fd, 0, SEEK_SET);

	ck_assert(!ebml_readsint(fd, 0x82, &num));
	ck_assert_int_eq(lseek(fd, 0, SEEK_CUR), 0);

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

#test-loop(0, 4) ebml_readuint_reads_uint
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

#test ebml_readuint_returns_zero_for_zero_length
	uint8_t data[] = { 0xfb, 1 << 7 | 0 };
	uint64_t num = UINT64_MAX; /* sentinel */

	write(fd, data, sizeof(data));
	lseek(fd, 0, SEEK_SET);

	ck_assert(ebml_readuint(fd, 0xfb, &num));
	ck_assert_int_eq(num, 0);
	ck_assert_int_eq(lseek(fd, 0, SEEK_CUR), sizeof(data));

#test ebml_readuint_returns_false_on_id_mismatch
	uint8_t data[] = { 0xfb, 1 << 7 | 1, 3 };
	uint64_t num;

	write(fd, data, sizeof(data));
	lseek(fd, 0, SEEK_SET);

	ck_assert(!ebml_readuint(fd, 0x82, &num));
	ck_assert_int_eq(lseek(fd, 0, SEEK_CUR), 0);

#test ebml_readuint_returns_false_when_value_too_large
	/* Value length 9 exceeds sizeof(uint64_t) = 8; must return false. */
	uint8_t data[] = { 0x82, 1 << 7 | 9, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	uint64_t num;

	write(fd, data, sizeof(data));
	lseek(fd, 0, SEEK_SET);

	ck_assert(!ebml_readuint(fd, 0x82, &num));
	ck_assert_int_eq(lseek(fd, 0, SEEK_CUR), 0);

#tcase ebml_readfloat

struct {
	uint_least32_t id;
	double expect;
	size_t data_sz;
	uint8_t data[1 + 1 + 8]; /* ID + size + up to 8 value bytes. */
} ebml_readfloat_cases[] = {
	/* 32-bit 1.0: IEEE 754 big-endian 0x3f800000. */
	{ 0xb5, 1.0,   6, { 0xb5, 1 << 7 | 4, 0x3f, 0x80, 0x00, 0x00 } },
	/* 64-bit 1.0: IEEE 754 big-endian 0x3ff0000000000000. */
	{ 0xb5, 1.0,  10, { 0xb5, 1 << 7 | 8,
	                    0x3f, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
	/* 32-bit -0.5: IEEE 754 big-endian 0xbf000000. */
	{ 0xb5, -0.5,  6, { 0xb5, 1 << 7 | 4, 0xbf, 0x00, 0x00, 0x00 } },
};

#test-loop(0, 3) ebml_readfloat_reads_float
	uint_least32_t id = ebml_readfloat_cases[_i].id;
	double expect = ebml_readfloat_cases[_i].expect;
	size_t data_sz = ebml_readfloat_cases[_i].data_sz;
	uint8_t *data = ebml_readfloat_cases[_i].data;

	double f;

	write(fd, data, data_sz);
	lseek(fd, 0, SEEK_SET);

	ck_assert(ebml_readfloat(fd, id, &f));
	ck_assert(f == expect);
	ck_assert_int_eq(lseek(fd, 0, SEEK_CUR), data_sz);

#test ebml_readfloat_handles_EBML_ANY_ELEMENT
	uint8_t data[] = { 0xb5, 1 << 7 | 4, 0x3f, 0x80, 0x00, 0x00 };
	double f;

	write(fd, data, sizeof(data));
	lseek(fd, 0, SEEK_SET);

	ck_assert(ebml_readfloat(fd, EAL, &f));
	ck_assert(f == 1.0);

#test ebml_readfloat_zero_length_returns_zero
	/* EBML spec: a zero-length float element has implied value 0.0. */
	uint8_t data[] = { 0xb5, 1 << 7 | 0 };
	double f = 999.0; /* sentinel */

	write(fd, data, sizeof(data));
	lseek(fd, 0, SEEK_SET);

	ck_assert(ebml_readfloat(fd, 0xb5, &f));
	ck_assert(f == 0.0);
	ck_assert_int_eq(lseek(fd, 0, SEEK_CUR), sizeof(data));

#test-signal(6) ebml_readfloat_aborts_on_size_not_0_32_64
	uint8_t data[] = { 0xb5, 1 << 7 | 3, 0x01, 0x02, 0x03 };
	double f;

	write(fd, data, sizeof(data));
	lseek(fd, 0, SEEK_SET);

	ebml_readfloat(fd, 0xb5, &f);

#tcase ebml_readstring

#test ebml_readstring_reads_string
	char *buf = nullptr;
	size_t sz = 0;
	uint8_t data[] = { 0xfb, 1 << 7 | 3, 'f', 'o', 'o' };

	write(fd, data, sizeof(data));
	lseek(fd, 0, SEEK_SET);

	ck_assert(ebml_readstring(fd, 0xfb, &buf, &sz));
	ck_assert_int_eq(sz, 3);
	ck_assert_mem_eq(buf, "foo", 3);
	ck_assert_int_eq(lseek(fd, 0, SEEK_CUR), sizeof(data));

	free(buf);

#test ebml_readstring_handles_EBML_ANY_ELEMENT
	char *buf = nullptr;
	size_t sz = 0;
	uint8_t data[] = { 0xfb, 1 << 7 | 3, 'f', 'o', 'o' };

	write(fd, data, sizeof(data));
	lseek(fd, 0, SEEK_SET);

	ck_assert(ebml_readstring(fd, EAL, &buf, &sz));
	ck_assert_int_eq(sz, 3);
	ck_assert_mem_eq(buf, "foo", 3);

	free(buf);

#test ebml_readstring_allocates_on_null_pointer
	char *buf = nullptr;
	size_t sz = 0;
	uint8_t data[] = { 0xfb, 1 << 7 | 5, 'h', 'e', 'l', 'l', 'o' };

	write(fd, data, sizeof(data));
	lseek(fd, 0, SEEK_SET);

	ck_assert(ebml_readstring(fd, 0xfb, &buf, &sz));
	ck_assert(buf != nullptr);
	ck_assert_int_eq(sz, 5);
	ck_assert_mem_eq(buf, "hello", 5);

	free(buf);

#test ebml_readstring_updates_sz_when_buffer_sufficient
	/* When the existing buffer is large enough, *sz must still be updated
	   to reflect the actual element length, not the buffer capacity. */
	char *buf = malloc(100);
	size_t sz = 100;
	uint8_t data[] = { 0xfb, 1 << 7 | 5, 'h', 'e', 'l', 'l', 'o' };

	write(fd, data, sizeof(data));
	lseek(fd, 0, SEEK_SET);

	ck_assert(ebml_readstring(fd, 0xfb, &buf, &sz));
	ck_assert_int_eq(sz, 5);

	free(buf);

#tcase ebml_readbinary

struct {
	uint_least32_t id;
	size_t data_sz;
	uint8_t data[sizeof(uint_least32_t) + VINT_OCTET_MAX + 8];
	uint8_t expect[8]; /* Expected buf contents: raw bytes, not reversed. */
} ebml_readbinary_cases[] = {
	{ 0xa1,       5, { 0xa1,             1 << 7 | 3, 0x01, 0x23, 0x45 },
	                 { 0x01, 0x23, 0x45 } },
	{ 0x1654ae6b, 6, { 0x16, 0x54, 0xae, 0x6b, 1 << 7 | 1, 0xff },
	                 { 0xff } },
	{ 0x4dbb,     3, { 0x4d, 0xbb, 1 << 7 | 0 },
	                 { 0 } },
};

#test-loop(0, 3) ebml_readbinary_reads_binary
	uint_least32_t id = ebml_readbinary_cases[_i].id;
	size_t data_sz = ebml_readbinary_cases[_i].data_sz;
	uint8_t *data = ebml_readbinary_cases[_i].data;
	uint8_t *expect = ebml_readbinary_cases[_i].expect;

	uint8_t buf[8];

	write(fd, data, data_sz);
	lseek(fd, 0, SEEK_SET);

	ck_assert(ebml_readbinary(fd, id, buf, sizeof(buf)));
	ck_assert_mem_eq(buf, expect, sizeof(buf));
	ck_assert_int_eq(lseek(fd, 0, SEEK_CUR), data_sz);

#test ebml_readbinary_returns_false_on_id_mismatch
	uint8_t data[] = { 0xa1, 1 << 7 | 3, 0x01, 0x23, 0x45 };
	uint8_t buf[8];

	write(fd, data, sizeof(data));
	lseek(fd, 0, SEEK_SET);

	ck_assert(!ebml_readbinary(fd, 0x82, buf, sizeof(buf)));
	ck_assert_int_eq(lseek(fd, 0, SEEK_CUR), 0);

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
