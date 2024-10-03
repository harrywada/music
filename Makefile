VINT_OCTET_MAX = 8
CFLAGS = -Wall -Wextra -Werror -ansi \
         -DVINT_OCTET_MAX=$(VINT_OCTET_MAX)

.SUFFIXES: _tests.ts _tests.c
_tests.ts_tests.c:
	checkmk $< >$@

LDFLAGS += -lcheck
ebml_tests: ebml_tests.c ebml.o utils.o utils_le.o
utils_tests: utils_tests.c utils.o
utils_le_tests: utils_le_tests.c utils_le.o
