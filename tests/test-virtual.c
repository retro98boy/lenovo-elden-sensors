// SPDX-License-Identifier: GPL-3.0-or-later
#include <math.h>
#include <string.h>
#include "virtual-decode.h"
#include "virtual.pb-c.h"

static void vectors(void)
{
	float values[] = { 1, 2, 3, 4, 5, 6 };
	VirtualVector msg = VIRTUAL_VECTOR__INIT;
	guint8 buf[128];
	GError *error = NULL;
	char *out;
	gsize len;

	msg.data = values;
	msg.n_data = 6;
	msg.accuracy = 3;
	len = virtual_vector__pack(&msg, buf);
	out = virtual_decode("gravity", 1025, buf, len, &error);
	g_assert_no_error(error);
	g_assert_nonnull(strstr(out, "\"gravity\":[1,2,3]"));
	g_assert_nonnull(strstr(out, "\"linear_acceleration\":[4,5,6]"));
	g_free(out);
	msg.n_data = 4;
	len = virtual_vector__pack(&msg, buf);
	const char *rotations[] = { "rotv", "game_rv", "geomag_rv" };
	for (guint n = 0; n < G_N_ELEMENTS(rotations); n++) {
		out = virtual_decode(rotations[n], 1025, buf, len, &error);
		g_assert_no_error(error);
		g_assert_nonnull(strstr(out, "\"quaternion_xyzw\":[1,2,3,4]"));
		g_free(out);
	}
	out = virtual_decode("gravity", 1025, buf, len, &error);
	g_assert_null(out);
	g_assert_nonnull(error);
	g_clear_error(&error);
	values[0] = NAN;
	len = virtual_vector__pack(&msg, buf);
	out = virtual_decode("rotv", 1025, buf, len, &error);
	g_assert_null(out);
	g_assert_nonnull(error);
	g_clear_error(&error);
}

static void events(void)
{
	const guint8 steps[] = { 8, 53 };
	const guint8 motion[] = { 8, 2 };
	GError *error = NULL;
	char *out = virtual_decode("pedometer", 1028, steps, sizeof(steps), &error);
	g_assert_no_error(error);
	g_assert_cmpstr(out, ==, "\"steps\":53,\"initial\":false");
	g_free(out);
	out = virtual_decode("pedometer", 775, steps, sizeof(steps), &error);
	g_assert_cmpstr(out, ==, "\"steps\":53,\"initial\":true");
	g_free(out);
	out = virtual_decode("amd", 772, motion, sizeof(motion), &error);
	g_assert_cmpstr(out, ==, "\"state\":\"motion\"");
	g_free(out);
	out = virtual_decode("rmd", 772, motion, sizeof(motion), &error);
	g_assert_cmpstr(out, ==, "\"state\":\"motion\"");
	g_free(out);
	const guint8 stationary[] = { 8, 1 };
	out = virtual_decode("amd", 772, stationary, sizeof(stationary), &error);
	g_assert_cmpstr(out, ==, "\"state\":\"stationary\"");
	g_free(out);
	const char *types[] = { "step_detect", "tilt", "sig_motion" };
	const guint ids[] = { 1029, 774, 772 };
	for (guint n = 0; n < G_N_ELEMENTS(types); n++) {
		out = virtual_decode(types[n], ids[n], NULL, 0, &error);
		g_assert_no_error(error);
		g_assert_cmpstr(out, ==, "\"triggered\":true");
		g_free(out);
	}
	out = virtual_decode("sig_motion", 772, motion, sizeof(motion), &error);
	g_assert_null(out);
	g_assert_nonnull(error);
	g_clear_error(&error);
	out = virtual_decode("pedometer", 1028, steps, 1, &error);
	g_assert_null(out);
	g_assert_nonnull(error);
	g_clear_error(&error);
	out = virtual_decode("gravity", 768, NULL, 0, &error);
	g_assert_null(out);
	g_assert_no_error(error);
}

static void captured_gravity(void)
{
	/* A stationary lenovo-elden report, including packed linear acceleration. */
	const guint8 payload[] = {
		0x0a, 0x18, 0xb0, 0xea, 0x31, 0xbf, 0x95, 0xf6, 0x97, 0x3f,
		0xda, 0x5a, 0x1b, 0x41, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0x10, 0x03,
	};
	GError *error = NULL;
	char *out = virtual_decode("gravity", 1025, payload, sizeof(payload), &error);
	g_assert_no_error(error);
	g_assert_nonnull(strstr(out, "\"accuracy\":3"));
	g_assert_nonnull(strstr(out, "\"linear_acceleration\":[0,0,0]"));
	g_free(out);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/virtual/vectors", vectors);
	g_test_add_func("/virtual/events", events);
	g_test_add_func("/virtual/captured-gravity", captured_gravity);
	return g_test_run();
}
