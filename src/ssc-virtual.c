// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * On-demand SSC subscriptions. stdout contains newline-delimited JSON.
 * No desktop policy, persistent step storage, or automatic one-shot rearming.
 */
#include <glib-unix.h>
#include <libssc.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include "virtual-decode.h"

struct subscription {
	const char *type;
	SSCSensor *sensor;
	GObject *client;
	guint64 high, low, samples;
	gulong handler;
	gboolean selected;
};

static struct subscription subscriptions[] = {
	{ .type = "gravity" }, { .type = "rotv" },
	{ .type = "game_rv" }, { .type = "geomag_rv" },
	{ .type = "pedometer" }, { .type = "step_detect" },
	{ .type = "sig_motion" }, { .type = "tilt" },
	{ .type = "amd" }, { .type = "rmd" },
};
static GMainLoop *loop;
static GCancellable *cancel;
static gboolean stopping;
static int status;
static guint closing;
static gboolean stop(gpointer data);

struct event {
	struct subscription *sub;
	char *fields;
	gint64 received;
	gboolean invalid;
};

static gboolean emit_event(gpointer data)
{
	struct event *event = data;
	struct subscription *sub = event->sub;

	if (!stopping) {
		if (event->invalid) {
			g_printerr("%s: malformed payload\n", sub->type);
			status = 1;
		} else {
			sub->samples++;
			g_print("{\"sensor\":\"%s\",\"received_monotonic_us\":%"
				G_GINT64_FORMAT ",%s}\n",
				sub->type, event->received, event->fields);
			if (ferror(stdout)) {
				status = 1;
				stop(NULL);
			}
		}
	}
	return G_SOURCE_REMOVE;
}

static void free_event(gpointer data)
{
	struct event *event = data;
	g_free(event->fields);
	g_free(event);
}

static void report(GObject *client, guint32 id, guint64 high, guint64 low,
		   GArray *buf, gpointer data)
{
	struct subscription *sub = data;
	GError *error = NULL;
	struct event *event;
	char *fields;
	(void)client;

	if (high != sub->high || low != sub->low)
		return;
	fields = virtual_decode(sub->type, id, buf->data, buf->len, &error);
	if (!fields && !error)
		return;
	event = g_new0(struct event, 1);
	event->sub = sub;
	event->fields = fields;
	event->received = g_get_monotonic_time();
	event->invalid = error != NULL;
	g_clear_error(&error);
	/* libssc delivers reports from its worker thread. Serialize JSON output. */
	g_idle_add_full(G_PRIORITY_DEFAULT, emit_event, event, free_event);
}

static gboolean quit_loop(gpointer data)
{
	(void)data;
	g_main_loop_quit(loop);
	return G_SOURCE_REMOVE;
}

static void closed(GObject *object, GAsyncResult *result, gpointer data)
{
	GError *error = NULL;
	struct subscription *sub = data;

	if (!ssc_sensor_close_finish(SSC_SENSOR(object), result, &error)) {
		g_printerr("%s: close: %s\n", sub->type, error->message);
		g_clear_error(&error);
		status = 1;
	}
	if (--closing == 0)
		g_main_loop_quit(loop);
}

static gboolean stop(gpointer data)
{
	(void)data;
	if (stopping)
		return G_SOURCE_CONTINUE;
	stopping = TRUE;
	g_cancellable_cancel(cancel);
	for (gsize n = 0; n < G_N_ELEMENTS(subscriptions); n++) {
		struct subscription *sub = &subscriptions[n];
		if (!sub->sensor)
			continue;
		closing++;
		ssc_sensor_close(sub->sensor, NULL, closed, sub);
	}
	if (!closing)
		g_main_loop_quit(loop);
	else
		g_timeout_add_seconds(3, quit_loop, NULL);
	return G_SOURCE_CONTINUE;
}

static void opened(GObject *object, GAsyncResult *result, gpointer data)
{
	GError *error = NULL;
	struct subscription *sub = data;

	if (!ssc_sensor_open_finish(SSC_SENSOR(object), result, &error)) {
		if (!stopping) {
			g_printerr("%s: subscribe: %s\n", sub->type, error->message);
			status = 1;
		}
		g_clear_error(&error);
	}
}

static void discovered(GObject *object, GAsyncResult *result, gpointer data)
{
	struct subscription *sub = data;
	GError *error = NULL;
	SSCSensor *sensor;
	(void)object;

	sensor = ssc_sensor_new_finish(result, &error);
	if (stopping) {
		g_clear_object(&sensor);
		g_clear_error(&error);
		return;
	}
	if (!sensor) {
		g_printerr("%s: %s\n", sub->type,
			   error ? error->message : "not available");
		g_clear_error(&error);
		status = 1;
		return;
	}
	sub->sensor = sensor;
	g_object_get(sensor, SSC_SENSOR_UID_HIGH, &sub->high,
		     SSC_SENSOR_UID_LOW, &sub->low,
		     SSC_SENSOR_CLIENT, &sub->client, NULL);
	sub->handler = g_signal_connect(sub->client, "report", G_CALLBACK(report), sub);
	g_print("{\"sensor\":\"%s\",\"discovered\":true}\n", sub->type);
	ssc_sensor_open(sensor, cancel, opened, sub);
}

int main(int argc, char **argv)
{
	char *selection = NULL;
	int seconds = 30;
	gboolean list = FALSE;
	GError *error = NULL;
	GOptionEntry options[] = {
		{ "sensor", 's', 0, G_OPTION_ARG_STRING, &selection,
		  "SSC type, comma-separated types, or all", "TYPES" },
		{ "timeout", 't', 0, G_OPTION_ARG_INT, &seconds,
		  "Subscription duration (default 30; 0 until interrupted)", "SECONDS" },
		{ "list", 'l', 0, G_OPTION_ARG_NONE, &list,
		  "List supported types, without querying hardware", NULL },
		{ NULL, 0, 0, 0, NULL, NULL, NULL },
	};
	GOptionContext *options_ctx = g_option_context_new("- SSC virtual sensor JSON stream");
	g_option_context_add_main_entries(options_ctx, options, NULL);
	if (!g_option_context_parse(options_ctx, &argc, &argv, &error)) {
		g_printerr("%s\n", error->message);
		return 2;
	}
	if (list) {
		for (gsize n = 0; n < G_N_ELEMENTS(subscriptions); n++)
			g_print("%s\n", subscriptions[n].type);
		return 0;
	}
	if (!selection || !selection[0] || seconds < 0 || argc != 1) {
		g_printerr("Use --sensor TYPE[,TYPE] or --sensor all; see --help.\n");
		return 2;
	}
	char **types = g_strsplit(selection, ",", -1);
	for (char **type = types; *type; type++) {
		gboolean found = FALSE;
		for (gsize n = 0; n < G_N_ELEMENTS(subscriptions); n++) {
			if (!strcmp(*type, "all") || !strcmp(*type, subscriptions[n].type)) {
				subscriptions[n].selected = TRUE;
				found = TRUE;
			}
		}
		if (!found) {
			g_printerr("Unsupported sensor: %s\n", *type);
			return 2;
		}
	}
	g_strfreev(types);
	g_free(selection);
	g_option_context_free(options_ctx);
	setvbuf(stdout, NULL, _IOLBF, 0);
	/* A closed pipe must still release subscriptions. */
	signal(SIGPIPE, SIG_IGN);
	loop = g_main_loop_new(NULL, FALSE);
	cancel = g_cancellable_new();
	g_unix_signal_add(SIGINT, stop, NULL);
	g_unix_signal_add(SIGTERM, stop, NULL);
	for (gsize n = 0; n < G_N_ELEMENTS(subscriptions); n++)
		if (subscriptions[n].selected)
			ssc_sensor_new((char *)subscriptions[n].type, cancel, discovered,
				       &subscriptions[n]);
	if (seconds)
		g_timeout_add_seconds(seconds, stop, NULL);
	g_main_loop_run(loop);
	for (gsize n = 0; n < G_N_ELEMENTS(subscriptions); n++) {
		struct subscription *sub = &subscriptions[n];
		if (sub->selected)
			g_printerr("%s: %" G_GUINT64_FORMAT " reports\n",
				   sub->type, sub->samples);
	}
	/* Process exit also closes SSC clients after a DSP disconnect. */
	return status;
}
