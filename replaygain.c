#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "replaygain.h"

static int8_t
clamp_s8(double v)
{
	if (v > INT8_MAX)
		return INT8_MAX;
	if (v < INT8_MIN)
		return INT8_MIN;
	return (int8_t) lrint(v);
}

static int16_t
clamp_s16(double v)
{
	if (v > INT16_MAX)
		return INT16_MAX;
	if (v < INT16_MIN)
		return INT16_MIN;
	return (int16_t) lrint(v);
}

struct replaygain
replaygain_from_tags(double gain_db, double peak, int has_peak)
{
	struct replaygain rg = {
		.gain = pow(10.0, gain_db / 20.0),
	};

	if (has_peak && peak > 0.0 && rg.gain * peak > 1.0)
		rg.gain = 1.0 / peak;

	return rg;
}

void
replaygain_apply_s8(int8_t *samples, size_t n, struct replaygain rg)
{
	if (rg.gain == 1.0)
		return;
	for (size_t i = 0; i < n; i++)
		samples[i] = clamp_s8((double) samples[i] * rg.gain);
}

void
replaygain_apply_s16(int16_t *samples, size_t n, struct replaygain rg)
{
	if (rg.gain == 1.0)
		return;
	for (size_t i = 0; i < n; i++)
		samples[i] = clamp_s16((double) samples[i] * rg.gain);
}
