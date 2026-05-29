# User-customizable values
TRACKS_MAX    = 4
VINT_OCTET_MAX = 8
MPRIS         = 1

CFLAGS += -std=c23 -D_POSIX_SOURCE -D_POSIX_C_SOURCE=200809L -DPLAY_PATH=\"./play\" \
          -Wall -Wextra -Werror -Wno-empty-body \
          -isystem /usr/include/alsa/ \
          -DTRACKS_MAX="$(TRACKS_MAX)" \
          -DVINT_OCTET_MAX="$(VINT_OCTET_MAX)"

ifeq ($(MPRIS),1)
MPRIS_OBJ    = mpris.o
MPRIS_CFLAGS = -DMPRIS -isystem /usr/include/elogind
MPRIS_LFLAGS = -lelogind
endif

play: LDFLAGS += -lasound
play: play.c ebml.o matroska.o utils.o
musicd: musicd.c cmds.o state.o queue.o song.o utils.o $(MPRIS_OBJ) tags.o matroska.o ebml.o
musicd: CFLAGS  += $(MPRIS_CFLAGS)
musicd: LDFLAGS += $(MPRIS_LFLAGS)
sq: sq.c ebml.o matroska.o song.o utils.o
sf: sf.c filter.o tags.o ebml.o matroska.o song.o utils.o
sc: sc.c utils.o
sp: sp.c format.o tags.o ebml.o matroska.o song.o utils.o

.SUFFIXES: _tests.ts _tests.c
_tests.ts_tests.c:
	checkmk $< >$@

LDFLAGS += -lcheck
cmds_tests: cmds_tests.c cmds.o state.o queue.o song.o utils.o ebml.o matroska.o
filter_tests: filter_tests.c filter.o tags.o ebml.o utils.o
format_tests: format_tests.c format.o tags.o ebml.o utils.o
ebml_tests: ebml_tests.c ebml.o utils.o
matroska_tests: matroska_tests.c matroska.o ebml.o utils.o
tags_tests: tags_tests.c tags.o ebml.o matroska.o utils.o
queue_tests: queue_tests.c queue.o song.o utils.o ebml.o matroska.o
song_tests: song_tests.c song.o utils.o ebml.o matroska.o
utils_tests: utils_tests.c utils.o

.PHONY: clean
clean:
	rm -rf musicd play sq sf sc sp *_tests *_tests.c *.o
