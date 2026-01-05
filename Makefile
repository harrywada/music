# User-customizable values
TRACKS_MAX = 4
VINT_OCTET_MAX = 8

CFLAGS += -Wall -Wextra -Werror -Wno-empty-body \
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
ebml_tests: ebml_tests.c ebml.o utils.o utils_le.o
matroska_tests: matroska_tests.c matroska.o
utils_tests: utils_tests.c utils.o
utils_le_tests: utils_le_tests.c utils_le.o

.PHONY: clean
clean:
	rm -rf musicd play *_tests *.o
