// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <glib.h>

/* Returns JSON fields, NULL for unrelated events, or sets error for invalid data. */
char *virtual_decode(const char *type, guint32 id, const void *data,
		     gsize len, GError **error);
