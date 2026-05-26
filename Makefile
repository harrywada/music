# User-customizable values
TRACKS_MAX = 4
VINT_OCTET_MAX = 8

CFLAGS += -std=c23 -D_POSIX_SOURCE -D_POSIX_C_SOURCE=200112L \
          -Wall -Wextra -Werror -Wno-empty-body \
          -isystem /usr/include/alsa/ \
          -DTRACKS_MAX="$(TRACKS_MAX)" \
          -DVINT_OCTET_MAX="$(VINT_OCTET_MAX)"

LDFLAGS += -lasound
play: play.c ebml.o matroska.o utils.o utils_le.o
musicd: musicd.c ebml.o matroska.o utils.o

.SUFFIXES: _tests.ts _tests.c
_tests.ts_tests.c:
	checkmk $< >$@

LDFLAGS += -lcheck
cmds_tests: cmds_tests.c cmds.o state.o
ebml_tests: ebml_tests.c ebml.o utils.o utils_le.o
matroska_tests: matroska_tests.c matroska.o ebml.o utils.o utils_le.o
queue_tests: queue_tests.c queue.o song.o utils.o
song_tests: song_tests.c song.o utils.o
utils_tests: utils_tests.c utils.o
utils_le_tests: utils_le_tests.c utils_le.o

.PHONY: clean
clean:
	rm -rf musicd play *_tests *.o
