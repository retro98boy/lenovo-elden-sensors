// SPDX-License-Identifier: GPL-3.0-or-later
#include <math.h>
#include <string.h>
#include "virtual.pb-c.h"
#include "virtual-decode.h"

static char *invalid(GError **error)
{
	g_set_error_literal(error, g_quark_from_static_string("ssc-virtual"), 1,
			    "Invalid sensor payload");
	return NULL;
}

char *virtual_decode(const char *type, guint32 id, const void *data,
		     gsize len, GError **error)
{
	gboolean gravity = !strcmp(type, "gravity");
	gboolean rotation = !strcmp(type, "rotv") || !strcmp(type, "game_rv") ||
			    !strcmp(type, "geomag_rv");
	gboolean steps = !strcmp(type, "pedometer");
	gboolean motion = !strcmp(type, "amd") || !strcmp(type, "rmd");
	VirtualVector *v;
	VirtualInteger *i;
	GString *out;
	char *result;

	if ((gravity || rotation) && id == 1025) {
		v = virtual_vector__unpack(NULL, len, data);
		if (!v)
			return invalid(error);
		if (v->n_data != (gravity ? 6U : 4U) ||
		    v->accuracy < 0 || v->accuracy > 3) {
			virtual_vector__free_unpacked(v, NULL);
			return invalid(error);
		}
		out = g_string_new(NULL);
		g_string_append_printf(out, "\"accuracy\":%d,\"unit\":\"%s\",",
				       v->accuracy, gravity ? "m/s^2" : "quaternion");
		g_string_append(out, gravity ? "\"gravity\":[" : "\"quaternion_xyzw\":[");
		for (gsize n = 0; n < v->n_data; n++) {
			char number[G_ASCII_DTOSTR_BUF_SIZE];

			if (!isfinite(v->data[n])) {
				g_string_free(out, TRUE);
				virtual_vector__free_unpacked(v, NULL);
				return invalid(error);
			}
			if (gravity && n == 3)
				g_string_append(out, "],\"linear_acceleration\":[");
			else if (n)
				g_string_append_c(out, ',');
			g_ascii_dtostr(number, sizeof(number), v->data[n]);
			g_string_append(out, number);
		}
		g_string_append_c(out, ']');
		virtual_vector__free_unpacked(v, NULL);
		return g_string_free(out, FALSE);
	}
	if ((steps && (id == 775 || id == 1028)) || (motion && id == 772)) {
		i = virtual_integer__unpack(NULL, len, data);
		if (!i)
			return invalid(error);
		if (motion && i->value > 2) {
			virtual_integer__free_unpacked(i, NULL);
			return invalid(error);
		}
		if (steps)
			result = g_strdup_printf("\"steps\":%u,\"initial\":%s",
						 i->value, id == 775 ? "true" : "false");
		else
			result = g_strdup_printf("\"state\":\"%s\"",
						 i->value == 0 ? "unknown" :
						 i->value == 1 ? "stationary" : "motion");
		virtual_integer__free_unpacked(i, NULL);
		return result;
	}
	if ((!strcmp(type, "step_detect") && id == 1029) ||
	    (!strcmp(type, "tilt") && id == 774) ||
	    (!strcmp(type, "sig_motion") && id == 772)) {
		if (len)
			return invalid(error);
		return g_strdup("\"triggered\":true");
	}
	/* Configuration and unrelated reports are not measurements. */
	return NULL;
}
