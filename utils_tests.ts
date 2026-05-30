#include <fcntl.h> /* O_ constants. */
#include <sys/mman.h> /* shm_open(3), shm_unlink(3). */
#include <unistd.h> /* close(2). */

#include <sys/types.h>
#include "utils.h"

#tcase pos

#test pos_returns_offset
	char data[] = "test data";
	int fd;

	fd = shm_open("test_pos_returns_offset", O_RDWR|O_CREAT, 0600);
	ck_assert_int_eq(pos(fd), 0);

	write(fd, data, sizeof(data));
	ck_assert_int_eq(pos(fd), sizeof(data));

	close(fd);
	shm_unlink("test_pos_returns_offset");


#test-signal(6) pos_aborts_on_error
	int fd = -1;
	pos(fd);

#tcase seek

#test seek_seeks_to_offset
	char data[] = "test data";
	int fd;

	fd = shm_open("test_seek_seeks_to_offset", O_RDWR|O_CREAT, 0600);
	write(fd, data, sizeof(data));

	ck_assert_int_eq(seek(fd, 0), 0);
	ck_assert_int_eq(lseek(fd, 0, SEEK_CUR), 0);

	ck_assert_int_eq(seek(fd, 3), 3);
	ck_assert_int_eq(lseek(fd, 0, SEEK_CUR), 3);

	close(fd);
	shm_unlink("test_seek_seeks_to_offset");

#test-signal(6) seek_aborts_on_error
	int fd = -1;
	seek(fd, 0);
