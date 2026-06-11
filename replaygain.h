#pragma once

#include <stddef.h>
#include <stdint.h>

struct replaygain {
	double gain;
};

[[gnu::const]]
struct replaygain replaygain_from_tags(double gain_db, double peak,
    int has_peak);
void replaygain_apply_s8(int8_t *, size_t, struct replaygain);
void replaygain_apply_s16(int16_t *, size_t, struct replaygain);
