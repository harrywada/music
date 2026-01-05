#include <stdint.h>
#include <sys/types.h>
#include "matroska.h"

#tcase mkv_readseekinfo

struct {
	size_t size;
	uint8_t data[TODO];
} test_mkv_readseekinfo_cases {

};

#test-loop(0, TODO) mkv_readseekinfo_reads_seekinfo
	struct mkv_seekinfo sk_info;
	int fd;

	uint8_t (* data)[TODO] = &test_mkv_readseekinfo_cases[_i].data;
	size_t expected_size = test_mkv_readseekinfo_cases[_i].size;

	fd = shm_open("test_mkv_readseekinfo_reads_seekinfo", O_RDWR|O_CREAT, 0600);
	write(fd, *data, size);
	lseek(fd, 0, SEEK_SET);

	ck_assert(mkv_readseekinfo(fd, &sk_info));
	/* TODO */

	close(fd);
	shm_unlink("test_mkv_readseekinfo_reads_seekinfo");

struct {
	size_t size;
	uint8_t data[TODO];
} test_mkv_readseekinfo_error_cases {

};

#test mkv_readseekinfo_returns_false_on_error
	struct mkv_seekinfo sk_info;
	int fd;

	uint8_t (* data)[TODO] = &test_mkv_readseekinfo_cases[_i].data;
	size_t expected_size = test_mkv_readseekinfo_cases[_i].size;

	fd = shm_open("test_mkv_readseekinfo_returns_false_on_error", O_RDWR|O_CREAT, 0600);
	write(fd, *data, size);
	lseek(fd, 0, SEEK_SET);

	ck_assert(mkv_readseekinfo(fd, &sk_info));
	/* TODO */

	close(fd);
	shm_unlink("test_mkv_readseekinfo_returns_false_on_error");

#test mkv_readseekinfo_returns_false_on_invalid_fd
	struct mkv_readseekinfo sk_info;
	int fd = -1;
	ck_assert(!mkv_readseekinfo(fd, &sk_info));

#tcase mkv_readinfo

struct {

} test_mkv_readinfo_cases {

};

#test mkv_readinfo_reads_info
	struct mkv_readinfo info;
	int fd;

	uint8_t (* data)[TODO] = &test_mkv_readseekinfo_cases[_i].data;
	size_t expected_size = test_mkv_readseekinfo_cases[_i].size;

	fd = shm_open("test_mkv_readseekinfo_reads_seekinfo", O_RDWR|O_CREAT, 0600);
	write(fd, *data, size);
	lseek(fd, 0, SEEK_SET);

	ck_assert(mkv_readseekinfo(fd, &info));
	/* TODO */

	close(fd);
	shm_unlink("test_mkv_readseekinfo_reads_seekinfo");

#test mkv_readinfo_returns_false_on_error

#test mkv_readinfo_returns_false_on_invalid_fd
	struct mkv_readinfo info;
	int fd = -1;
	ck_assert(!mkv_readinfo(fd, &info));

#tcase mkv_readcluster

#test mkv_readcluster_reads_cluster

#test mkv_readcluster_returns_false_on_error

#test mkv_readcluster_returns_false_on_invalid_fd
	struct mkv_readcluster cluster;
	int fd = -1;
	ck_assert(!mkv_readcluster(fd, &cluster));

#tcase mkv_readtrackentry

#test mkv_readtrackentry_reads_trackentry

#test mkv_readtrackentry_returns_false_on_error

#test mkv_readtrackentry_returns_false_on_invalid_fd
	struct mkv_readtrackentry track;
	int fd = -1;
	ck_assert(!mkv_readtrackentry(fd, &track));

#tcase mkv_readcuepoint

#test mkv_readcuepoint_reads_cuepoint

#test mkv_readcuepoint_returns_false_on_error

#test mkv_readcuepoint_returns_false_on_invalid_fd
	struct mkv_readcuepoint cue;
	int fd = -1;
	ck_assert(!mkv_readcuepoint(fd, &cue));

#tcase mkv_readblock

#test mkv_readblock_reads_block

#test mkv_readblock_returns_false_on_error

#test mkv_readblock_returns_false_on_invalid_fd
	struct mkv_readblock block;
	int fd = -1;
	ck_assert(!mkv_readblock(fd, &block));

#tcase mkv_readchapteratom

#test mkv_readchapteratom_reads_chapteratom

#test mkv_readchapteratom_returns_false_on_error

#test mkv_readchapteratom_returns_false_on_invalid_fd
	struct mkv_readchapteratom chapter;
	int fd = -1;
	ck_assert(!mkv_readchapteratom(fd, &chapter));
