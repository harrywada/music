#include <stddef.h>
#include "utils_le.h"

#tcase rev_octets

struct {
	size_t size;
	uint8_t data[8];
	uint8_t expect[8];
} rev_octets_cases[] = {
	{ 7, { 1, 2, 3, 4, 5, 6, 7 },
	     { 7, 6, 5, 4, 3, 2, 1 }},
	{ 8, { 8, 7, 6, 5, 4, 3, 2, 1 },
	     { 1, 2, 3, 4, 5, 6, 7, 8 }},
	{ 1, { 12 }, { 12 }},
};

#test-loop(0, 3) rev_octets_revs_octets
	size_t size = rev_octets_cases[_i].size;
	uint8_t *data = rev_octets_cases[_i].data;
	uint8_t *expect = rev_octets_cases[_i].expect;

	rev_octets(data, size);
	ck_assert_mem_eq(data, expect, size);
