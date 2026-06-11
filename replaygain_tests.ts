#include <math.h>
#include <stdint.h>

#include "replaygain.h"

#suite replaygain

#tcase replaygain_from_tags

#test from_tags_converts_db_to_linear_gain
	struct replaygain rg = replaygain_from_tags(-6.0, 0.0, 0);

	ck_assert_double_eq_tol(rg.gain, pow(10.0, -6.0 / 20.0), 0.000001);

#test from_tags_caps_gain_using_peak
	struct replaygain rg = replaygain_from_tags(6.0, 0.75, 1);

	ck_assert_double_eq_tol(rg.gain, 1.0 / 0.75, 0.000001);

#test from_tags_ignores_invalid_peak
	struct replaygain rg = replaygain_from_tags(6.0, 0.0, 1);

	ck_assert_double_eq_tol(rg.gain, pow(10.0, 6.0 / 20.0), 0.000001);

#tcase replaygain_apply

#test apply_s8_scales_and_clamps_samples
	int8_t samples[] = { 64, -64, 100, -100 };
	struct replaygain rg = { .gain = 2.0 };

	replaygain_apply_s8(samples, 4, rg);

	ck_assert_int_eq(samples[0], 127);
	ck_assert_int_eq(samples[1], -128);
	ck_assert_int_eq(samples[2], 127);
	ck_assert_int_eq(samples[3], -128);

#test apply_s16_scales_and_clamps_samples
	int16_t samples[] = { 1000, -1000, 30000, -30000 };
	struct replaygain rg = { .gain = 2.0 };

	replaygain_apply_s16(samples, 4, rg);

	ck_assert_int_eq(samples[0], 2000);
	ck_assert_int_eq(samples[1], -2000);
	ck_assert_int_eq(samples[2], 32767);
	ck_assert_int_eq(samples[3], -32768);

#test apply_gain_one_leaves_samples_unchanged
	int16_t samples[] = { 100, -100 };
	struct replaygain rg = { .gain = 1.0 };

	replaygain_apply_s16(samples, 2, rg);

	ck_assert_int_eq(samples[0], 100);
	ck_assert_int_eq(samples[1], -100);
