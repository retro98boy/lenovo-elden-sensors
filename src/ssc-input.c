// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Lenovo Y700 Gen4 SSC motion and lid input devices.
 * Based on the sensor interfaces used by h0cheung/tb322fc-linux.
 */
#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <libssc.h>
#include <linux/uinput.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "hall.pb-c.h"

struct bridge {
	GMainLoop *loop;
	GCancellable *cancel;
	int fd;
	int status;
	int lid;
	gboolean hall_mode;
	gboolean accel_open;
	gboolean gyro_open;
	gboolean hall_open;
	SSCSensorAccelerometer *accel;
	SSCSensorGyroscope *gyro;
	SSCSensor *hall;
	SSCClient *client;
	gulong report_id;
	guint64 uid_high, uid_low;
	gint64 last_accel, last_gyro;
};

static gboolean quit_loop(gpointer data)
{
	struct bridge *b = data;

	g_cancellable_cancel(b->cancel);
	g_main_loop_quit(b->loop);
	return G_SOURCE_CONTINUE;
}

static void fail(struct bridge *b, const char *operation, const GError *error)
{
	g_printerr("%s: %s\n", operation, error ? error->message : "failed");
	b->status = 1;
	g_main_loop_quit(b->loop);
}

static gboolean emit(struct bridge *b, struct input_event *events, size_t count)
{
	ssize_t ret;
	size_t size = count * sizeof(*events);

	do {
		ret = write(b->fd, events, size);
	} while (ret < 0 && errno == EINTR);
	if (ret != (ssize_t)size) {
		fail(b, "Writing input events", NULL);
		return FALSE;
	}
	return TRUE;
}

static gboolean setup_input(struct bridge *b)
{
	struct uinput_setup setup = {
		.id = { .bustype = BUS_VIRTUAL, .vendor = 0x17aa,
			.product = b->hall_mode ? 2 : 1, .version = 1 },
	};
	const int axes[] = { ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ };

	b->fd = open("/dev/uinput", O_WRONLY | O_CLOEXEC);
	if (b->fd < 0)
		return FALSE;
	if (b->hall_mode) {
		g_strlcpy(setup.name, "Lenovo Y700 Gen4 Hall", sizeof(setup.name));
		if (ioctl(b->fd, UI_SET_EVBIT, EV_SW) ||
		    ioctl(b->fd, UI_SET_SWBIT, SW_LID))
			return FALSE;
	} else {
		g_strlcpy(setup.name, "Lenovo Y700 Gen4 Motion", sizeof(setup.name));
		if (ioctl(b->fd, UI_SET_EVBIT, EV_ABS) ||
		    ioctl(b->fd, UI_SET_PROPBIT, INPUT_PROP_ACCELEROMETER))
			return FALSE;
		for (size_t i = 0; i < G_N_ELEMENTS(axes); i++) {
			struct uinput_abs_setup abs = {
				.code = axes[i],
				.absinfo = {
					.minimum = i < 3 ? -4096 : -65536,
					.maximum = i < 3 ? 4096 : 65536,
					.resolution = i < 3 ? 26 : 1024,
				},
			};
			if (ioctl(b->fd, UI_SET_ABSBIT, axes[i]) ||
			    ioctl(b->fd, UI_ABS_SETUP, &abs))
				return FALSE;
		}
	}
	if (ioctl(b->fd, UI_DEV_SETUP, &setup) ||
	    ioctl(b->fd, UI_DEV_CREATE))
		return FALSE;
	g_print("Created %s\n", setup.name);
	return TRUE;
}

static void emit_motion(struct bridge *b, int base, float x, float y, float z)
{
	float scale = base == ABS_X ? 256.0f / 9.80665f : 1024.0f;
	float limit = base == ABS_X ? 4096.0f : 65536.0f;
	float values[] = { x, y, z };
	struct input_event events[4] = {0};

	/* The proxy's evdev backend expects 256 units per g. Gyro uses rad/s. */
	for (int i = 0; i < 3; i++) {
		if (!isfinite(values[i]))
			return;
		events[i].type = EV_ABS;
		events[i].code = base + i;
		events[i].value = lrintf(CLAMP(values[i] * scale, -limit, limit));
	}
	events[3].type = EV_SYN;
	events[3].code = SYN_REPORT;
	emit(b, events, G_N_ELEMENTS(events));
}

static void accel_measurement(SSCSensorAccelerometer *sensor, float x,
			      float y, float z, gpointer data)
{
	struct bridge *b = data;
	(void)sensor;
	b->last_accel = g_get_monotonic_time();
	emit_motion(b, ABS_X, x, y, z);
}

static void gyro_measurement(SSCSensorGyroscope *sensor, float x,
			     float y, float z, gpointer data)
{
	struct bridge *b = data;
	(void)sensor;
	b->last_gyro = g_get_monotonic_time();
	emit_motion(b, ABS_RX, x, y, z);
}

static gboolean motion_watchdog(gpointer data)
{
	struct bridge *b = data;
	gint64 now = g_get_monotonic_time();

	if (now - b->last_accel > 15 * G_USEC_PER_SEC ||
	    now - b->last_gyro > 15 * G_USEC_PER_SEC)
		fail(b, "SSC motion stream stopped", NULL);
	return G_SOURCE_CONTINUE;
}

static gboolean publish_lid(gpointer data)
{
	struct bridge *b = data;
	struct input_event events[] = {
		{ .type = EV_SW, .code = SW_LID, .value = b->lid },
		{ .type = EV_SYN, .code = SYN_REPORT },
	};

	if (emit(b, events, G_N_ELEMENTS(events)))
		g_debug("Hall state: %s", b->lid ? "closed" : "open");
	return G_SOURCE_REMOVE;
}

struct lid_event {
	struct bridge *bridge;
	int state;
};

static gboolean update_lid(gpointer data)
{
	struct lid_event *event = data;
	struct bridge *b = event->bridge;

	if (event->state != b->lid) {
		b->lid = event->state;
		publish_lid(b);
	}
	return G_SOURCE_REMOVE;
}

static void hall_report(SSCClient *client, guint32 id, guint64 high,
			guint64 low, GArray *buffer, gpointer data)
{
	struct bridge *b = data;
	HallEvent *message;
	struct lid_event *event;
	(void)client;

	if (id != 770 || high != b->uid_high || low != b->uid_low || !buffer)
		return;
	message = hall_event__unpack(NULL, buffer->len, (guint8 *)buffer->data);
	if (!message)
		return;
	if (message->state <= 1) {
		event = g_new(struct lid_event, 1);
		event->bridge = b;
		event->state = message->state;
		/* SSC emits reports on its worker thread. Serialize input writes. */
		g_idle_add_full(G_PRIORITY_DEFAULT, update_lid, event, g_free);
	}
	hall_event__free_unpacked(message, NULL);
}

static void hall_opened(GObject *source, GAsyncResult *result, gpointer data)
{
	struct bridge *b = data;
	g_autoptr(GError) error = NULL;

	b->hall_open = ssc_sensor_open_finish(SSC_SENSOR(source), result, &error);
	if (!b->hall_open)
		fail(b, "Opening hall sensor", error);
}

static void hall_created(GObject *source, GAsyncResult *result, gpointer data)
{
	struct bridge *b = data;
	g_autoptr(GError) error = NULL;
	(void)source;

	b->hall = ssc_sensor_new_finish(result, &error);
	if (!b->hall) {
		fail(b, "Finding hall sensor", error);
		return;
	}
	g_object_get(b->hall, SSC_SENSOR_CLIENT, &b->client,
		     SSC_SENSOR_UID_HIGH, &b->uid_high,
		     SSC_SENSOR_UID_LOW, &b->uid_low, NULL);
	b->report_id = g_signal_connect(b->client, "report", G_CALLBACK(hall_report), b);
	ssc_sensor_open(b->hall, b->cancel, hall_opened, b);
}

int main(int argc, char **argv)
{
	struct bridge b = { .fd = -1, .lid = -1 };
	g_autoptr(GError) error = NULL;
	guint sigint, sigterm, watchdog = 0;

	if (argc != 2 || (strcmp(argv[1], "motion") && strcmp(argv[1], "hall"))) {
		g_printerr("Usage: %s motion|hall\n", argv[0]);
		return 1;
	}
	b.hall_mode = !strcmp(argv[1], "hall");
	b.loop = g_main_loop_new(NULL, FALSE);
	b.cancel = g_cancellable_new();
	sigint = g_unix_signal_add(SIGINT, quit_loop, &b);
	sigterm = g_unix_signal_add(SIGTERM, quit_loop, &b);
	if (!setup_input(&b)) {
		g_printerr("Creating input device: %s\n", g_strerror(errno));
		b.status = 1;
		goto out;
	}
	if (b.hall_mode) {
		ssc_sensor_new("hall", b.cancel, hall_created, &b);
	} else {
		b.accel = ssc_sensor_accelerometer_new_sync(b.cancel, &error);
		if (!b.accel) {
			fail(&b, "Finding accelerometer", error);
			goto out;
		}
		b.gyro = ssc_sensor_gyroscope_new_sync(b.cancel, &error);
		if (!b.gyro) {
			fail(&b, "Finding gyroscope", error);
			goto out;
		}
		g_signal_connect(b.accel, "measurement", G_CALLBACK(accel_measurement), &b);
		g_signal_connect(b.gyro, "measurement", G_CALLBACK(gyro_measurement), &b);
		b.accel_open = ssc_sensor_accelerometer_open_sync(b.accel, b.cancel, &error);
		if (!b.accel_open) {
			fail(&b, "Opening accelerometer", error);
			goto out;
		}
		b.gyro_open = ssc_sensor_gyroscope_open_sync(b.gyro, b.cancel, &error);
		if (!b.gyro_open) {
			fail(&b, "Opening gyroscope", error);
			goto out;
		}
		b.last_accel = b.last_gyro = g_get_monotonic_time();
		watchdog = g_timeout_add_seconds(5, motion_watchdog, &b);
	}
	if (!g_cancellable_is_cancelled(b.cancel))
		g_main_loop_run(b.loop);
out:
	if (watchdog)
		g_source_remove(watchdog);
	g_source_remove(sigint);
	g_source_remove(sigterm);
	if (b.report_id)
		g_signal_handler_disconnect(b.client, b.report_id);
	/* Process exit releases the hall QMI client and its subscriptions. */
	if (b.gyro_open)
		ssc_sensor_gyroscope_close_sync(b.gyro, NULL, NULL);
	if (b.accel_open)
		ssc_sensor_accelerometer_close_sync(b.accel, NULL, NULL);
	if (b.fd >= 0) {
		ioctl(b.fd, UI_DEV_DESTROY);
		close(b.fd);
	}
	g_main_loop_unref(b.loop);
	g_object_unref(b.cancel);
	return b.status;
}
