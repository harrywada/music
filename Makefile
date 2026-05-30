# User-customizable values
TRACKS_MAX    = 4
VINT_OCTET_MAX = 8
MPRIS         = 1
PLAY_PATH     = ./play

CFLAGS += -std=c23 -D_POSIX_SOURCE -D_POSIX_C_SOURCE=200809L -DPLAY_PATH=\"$(PLAY_PATH)\" \
          -Wall -Wextra -Werror -Wno-empty-body \
          -isystem /usr/include/alsa/ \
          -DTRACKS_MAX="$(TRACKS_MAX)" \
          -DVINT_OCTET_MAX="$(VINT_OCTET_MAX)"

ifeq ($(MPRIS),1)
MPRIS_OBJ    = mpris.o tags.o
MPRIS_CFLAGS = -DMPRIS -isystem /usr/include/elogind
MPRIS_LFLAGS = -lelogind
endif

play: LDFLAGS += -lasound
play: play.c ebml.o matroska.o matroska_utils.o utils.o log_syslog.o
musicd: musicd.c cmds.o state.o queue.o song.o utils.o log_syslog.o $(MPRIS_OBJ) matroska.o matroska_utils.o ebml.o
musicd: CFLAGS  += $(MPRIS_CFLAGS)
musicd: LDFLAGS += $(MPRIS_LFLAGS)
sq: sq.c ebml.o matroska.o matroska_utils.o song.o utils.o log_stderr.o
sf: sf.c filter.o tags.o ebml.o matroska.o matroska_utils.o song.o utils.o log_stderr.o
sc: sc.c utils.o log_stderr.o
sp: sp.c format.o tags.o ebml.o matroska.o matroska_utils.o song.o utils.o log_stderr.o

.SUFFIXES: _tests.ts _tests.c
_tests.ts_tests.c:
	checkmk $< >$@

LDFLAGS += -lcheck
cmds_tests: cmds_tests.c cmds.o state.o queue.o song.o utils.o log_stderr.o ebml.o matroska.o matroska_utils.o
filter_tests: filter_tests.c filter.o tags.o ebml.o utils.o log_stderr.o
format_tests: format_tests.c format.o tags.o ebml.o utils.o log_stderr.o
ebml_tests: ebml_tests.c ebml.o utils.o log_stderr.o
matroska_tests: matroska_tests.c matroska.o ebml.o utils.o log_stderr.o
matroska_utils_tests: matroska_utils_tests.c matroska_utils.o matroska.o ebml.o utils.o log_stderr.o
tags_tests: tags_tests.c tags.o ebml.o matroska.o utils.o log_stderr.o
queue_tests: queue_tests.c queue.o song.o utils.o log_stderr.o ebml.o matroska.o matroska_utils.o
song_tests: song_tests.c song.o utils.o log_stderr.o ebml.o matroska.o matroska_utils.o
utils_tests: utils_tests.c utils.o log_stderr.o

BINDIR    = /usr/bin
LIBEXECDIR = /usr/libexec/music

.PHONY: install
install: play sq sf sc sp
	rm -f musicd musicd.o
	$(MAKE) musicd PLAY_PATH=$(LIBEXECDIR)/play
	install -Dm755 play $(LIBEXECDIR)/play
	install -m755 musicd sq sf sc sp $(BINDIR)/

.PHONY: clean
clean:
	rm -rf musicd play sq sf sc sp *_tests *_tests.c *.o log_syslog.o log_stderr.o
