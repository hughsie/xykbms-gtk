/*
 * Copyright 2026 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <string.h>

#include "xy-config.h"
#include "xy-device.h"

typedef enum {
	XY_JOB_READ_CONFIG,
	XY_JOB_WRITE_CONFIG,
	XY_JOB_WRITE_LAYERS,
	XY_JOB_WRITE_FIRMWARE,
	XY_JOB_RESTORE_CONFIG,
	XY_JOB_ENTER_BOOT,
	XY_JOB_BACKLIGHT,
	XY_JOB_VERSION,
	XY_JOB_BATTERY,
	XY_JOB_SCANCODES,
} XyJobKind;

typedef struct {
	const gchar *label;
	const gchar *tooltip;
	gsize offset;
	guint8 mask;
} XyFlagInfo;

static const XyFlagInfo xy_flags[] = {
    {"TrackPoint middle-button scroll", NULL, XY_CFG_OFF_FLAGS_A, XY_CFG_FLAG_A_TP_MIDDLE_SCROLL},
    {"Double-tap TP middle as Backspace",
     NULL,
     XY_CFG_OFF_FLAGS_A,
     XY_CFG_FLAG_A_TP_DOUBLE_MIDDLE_BKSP},
    {"Mouse middle button as Backspace",
     "Only for boards with USB",
     XY_CFG_OFF_FLAGS_A,
     XY_CFG_FLAG_A_MIDDLE_BKSP},
    {"Keep LEDs in low-power mode",
     "Only for boards without USB",
     XY_CFG_OFF_FLAGS_A,
     XY_CFG_FLAG_A_LOW_POWER_KEEP_LED},
    {"Disable Windows key", NULL, XY_CFG_OFF_FLAGS_A, XY_CFG_FLAG_A_DISABLE_WIN},
    {"TrackPoint on at power-up",
     "Only for boards without USB",
     XY_CFG_OFF_FLAGS_A,
     XY_CFG_FLAG_A_TP_ON_AT_POWER_UP},
    {"Invert TrackPoint X/Y", NULL, XY_CFG_OFF_FLAGS_A, XY_CFG_FLAG_A_TP_INVERT_XY},
    {"Allow low-power in USB mode", NULL, XY_CFG_OFF_FLAGS_A, XY_CFG_FLAG_A_USB_LOW_POWER},
    {"Disable STA LED blinking", NULL, XY_CFG_OFF_FLAGS_B, XY_CFG_FLAG_B_DISABLE_STA_BLINK},
    {"STA LED off when not blinking", NULL, XY_CFG_OFF_FLAGS_B, XY_CFG_FLAG_B_STA_OFF},
    {"Swap Fn and Left Ctrl", NULL, XY_CFG_OFF_FLAGS_B, XY_CFG_FLAG_B_SWAP_FN_LCTRL},
    {"Enable Bluetooth pairing passkey", NULL, XY_CFG_OFF_FLAGS_B, XY_CFG_FLAG_B_BT_PASSKEY},
    {"Only specific keys wake the keyboard",
     NULL,
     XY_CFG_OFF_FLAGS_B,
     XY_CFG_FLAG_B_SPECIFIC_WAKE},
};

/* index into xy_flags[] */
enum {
	XY_FLAG_TP_MIDDLE_SCROLL,
	XY_FLAG_TP_DOUBLE_MIDDLE_BKSP,
	XY_FLAG_MIDDLE_BKSP,
	XY_FLAG_LOW_POWER_KEEP_LED,
	XY_FLAG_DISABLE_WIN,
	XY_FLAG_TP_ON_AT_POWER_UP,
	XY_FLAG_TP_INVERT_XY,
	XY_FLAG_USB_LOW_POWER,
	XY_FLAG_DISABLE_STA_BLINK,
	XY_FLAG_STA_OFF,
	XY_FLAG_SWAP_FN_LCTRL,
	XY_FLAG_BT_PASSKEY,
	XY_FLAG_SPECIFIC_WAKE,
	XY_FLAG_LAST
};

static const gchar *xy_rf_modes[] = {"BT4", "USB", "URT", NULL};
static const gchar *xy_rf_channels[] = {"1", "2", "3", "4", NULL};

typedef struct {
	GtkApplication *application;
	GtkWindow *window;
	GtkWidget *options_box;
	GtkWidget *board_label;
	GtkWidget *bt_name;
	GtkWidget *auto_off;
	GtkWidget *tp_auto_off;
	GtkWidget *rf_mode;
	GtkWidget *rf_channel;
	GtkWidget *move_speed;
	GtkWidget *scroll_speed;
	GtkWidget *flags[XY_FLAG_LAST];
	GtkWidget *backlight;
	GtkWidget *update_pin_cfg;
	GtkWidget *progress;
	GtkWidget *log_view;
	GtkWidget *scan_button;
	GPtrArray *action_buttons; /* insensitive while busy */
	guint8 cfg[XY_CFG_SIZE];
	gboolean cfg_valid;
	gboolean busy;
	gint progress_pct; /* atomic */
	guint progress_id;
	guint backlight_id;
	GCancellable *scan_cancellable;
} XyApp;

typedef struct {
	XyApp *app;
	XyJobKind kind;
	guint8 cfg[XY_CFG_SIZE];
	guint8 *layers;
	GBytes *fw;
	guint16 fw_type;
	gboolean bluetooth;
	guint8 percentage;
	gchar *result;
	gchar *title;
} XyJob;

static void
xy_job_free(XyJob *job)
{
	g_free(job->layers);
	if (job->fw != NULL)
		g_bytes_unref(job->fw);
	g_free(job->result);
	g_free(job->title);
	g_free(job);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(XyJob, xy_job_free)

static XyJob *
xy_job_new(XyApp *app, XyJobKind kind, const gchar *title)
{
	XyJob *job = g_new0(XyJob, 1);
	job->app = app;
	job->kind = kind;
	job->title = g_strdup(title);
	return job;
}

/* ---------------------------------------------------------------------------
 * logging and messages
 */

static void
xy_app_log(XyApp *app, const gchar *fmt, ...) G_GNUC_PRINTF(2, 3);

static void
xy_app_log(XyApp *app, const gchar *fmt, ...)
{
	va_list args;
	GtkTextIter iter;
	g_autofree gchar *msg = NULL;
	g_autoptr(GDateTime) dt = g_date_time_new_now_local();
	g_autofree gchar *ts = g_date_time_format(dt, "%H:%M:%S");
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->log_view));

	va_start(args, fmt);
	msg = g_strdup_vprintf(fmt, args);
	va_end(args);
	gtk_text_buffer_get_end_iter(buffer, &iter);
	gtk_text_buffer_insert(buffer, &iter, ts, -1);
	gtk_text_buffer_insert(buffer, &iter, "  ", -1);
	gtk_text_buffer_insert(buffer, &iter, msg, -1);
	gtk_text_buffer_insert(buffer, &iter, "\n", -1);
	gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(app->log_view),
				     gtk_text_buffer_get_insert(buffer),
				     0.0,
				     FALSE,
				     0.0,
				     0.0);
}

static void
xy_app_message(XyApp *app, const gchar *title, const gchar *detail)
{
	g_autoptr(GtkAlertDialog) dialog = gtk_alert_dialog_new("%s", title);
	if (detail != NULL)
		gtk_alert_dialog_set_detail(dialog, detail);
	gtk_alert_dialog_show(dialog, app->window);
}

/* ---------------------------------------------------------------------------
 * config <-> widgets
 */

static void
xy_app_update_sensitivity(XyApp *app)
{
	guint8 board = app->cfg[XY_CFG_OFF_BOARD];
	gboolean tp_wireless = board == XY_BOARD_TP_WIRELESS;
	gboolean mech_wireless = board == XY_BOARD_MECH_WIRELESS;
	gboolean tp_mech = board == XY_BOARD_TP_MECH || board == XY_BOARD_TP60_MECH ||
			   board == XY_BOARD_TP60_MECH2;
	gboolean wired = board == XY_BOARD_WIRED_USB;
	gboolean sta_off;

	for (guint i = 0; i < app->action_buttons->len; i++)
		gtk_widget_set_sensitive(g_ptr_array_index(app->action_buttons, i), !app->busy);
	gtk_widget_set_sensitive(app->scan_button, !app->busy || app->scan_cancellable != NULL);
	gtk_widget_set_sensitive(app->options_box, app->cfg_valid && !app->busy);
	if (!app->cfg_valid)
		return;

	/* this mirrors what the vendor tool enables for each board type */
	gtk_widget_set_sensitive(app->bt_name, !wired);
	gtk_widget_set_sensitive(app->auto_off, !wired);
	gtk_widget_set_sensitive(app->rf_mode, !wired);
	gtk_widget_set_sensitive(app->rf_channel, !wired);
	gtk_widget_set_sensitive(app->tp_auto_off, !wired && !mech_wireless);
	gtk_widget_set_sensitive(app->move_speed, !mech_wireless);
	gtk_widget_set_sensitive(app->scroll_speed, !mech_wireless);
	gtk_widget_set_sensitive(app->flags[XY_FLAG_TP_MIDDLE_SCROLL], !mech_wireless);
	gtk_widget_set_sensitive(app->flags[XY_FLAG_TP_DOUBLE_MIDDLE_BKSP], !mech_wireless);
	gtk_widget_set_sensitive(app->flags[XY_FLAG_MIDDLE_BKSP],
				 !wired && !mech_wireless && !tp_mech);
	gtk_widget_set_sensitive(app->flags[XY_FLAG_LOW_POWER_KEEP_LED], !wired);
	gtk_widget_set_sensitive(app->flags[XY_FLAG_TP_INVERT_XY], !tp_wireless && !mech_wireless);
	gtk_widget_set_sensitive(app->flags[XY_FLAG_USB_LOW_POWER], !wired);
	gtk_widget_set_sensitive(app->flags[XY_FLAG_TP_ON_AT_POWER_UP],
				 !wired && !mech_wireless && !tp_mech);
	gtk_widget_set_sensitive(app->flags[XY_FLAG_DISABLE_STA_BLINK],
				 !wired && !mech_wireless && !tp_mech);
	sta_off = gtk_check_button_get_active(GTK_CHECK_BUTTON(app->flags[XY_FLAG_DISABLE_STA_BLINK]));
	gtk_widget_set_sensitive(app->flags[XY_FLAG_STA_OFF],
				 sta_off && !wired && !mech_wireless && !tp_mech);
	gtk_widget_set_sensitive(app->flags[XY_FLAG_SWAP_FN_LCTRL], !mech_wireless && !tp_mech);
	gtk_widget_set_sensitive(app->flags[XY_FLAG_BT_PASSKEY], !wired);
	gtk_widget_set_sensitive(app->flags[XY_FLAG_SPECIFIC_WAKE], !wired && !tp_wireless);
	gtk_widget_set_sensitive(app->backlight,
				 !app->busy && !tp_wireless && board != XY_BOARD_TP_MECH &&
				     board != XY_BOARD_TP60_MECH);
}

static void
xy_app_config_to_widgets(XyApp *app)
{
	const gchar *board = xy_board_type_to_string(app->cfg[XY_CFG_OFF_BOARD]);
	g_autofree gchar *board_str = NULL;
	g_autofree gchar *name = NULL;
	guint8 mode = app->cfg[XY_CFG_OFF_RF_MODE];
	guint8 channel = app->cfg[XY_CFG_OFF_RF_CHANNEL];

	if (board != NULL)
		board_str = g_strdup(board);
	else
		board_str = g_strdup_printf("Unknown board type %u", app->cfg[XY_CFG_OFF_BOARD]);
	gtk_label_set_text(GTK_LABEL(app->board_label), board_str);

	name = g_strndup((const gchar *)app->cfg + XY_CFG_OFF_BT_NAME, XY_CFG_BT_NAME_MAX + 1);
	gtk_editable_set_text(GTK_EDITABLE(app->bt_name),
			      g_utf8_validate(name, -1, NULL) ? name : "");
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->auto_off),
				  xy_config_get_u16(app->cfg, XY_CFG_OFF_AUTO_OFF));
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->tp_auto_off),
				  xy_config_get_u16(app->cfg, XY_CFG_OFF_TP_AUTO_OFF));
	if (mode >= XY_RF_MODE_BT4 && mode <= XY_RF_MODE_URT)
		gtk_drop_down_set_selected(GTK_DROP_DOWN(app->rf_mode), mode - XY_RF_MODE_BT4);
	else
		xy_app_log(app, "Unexpected wireless mode %u", mode);
	if (channel < 4)
		gtk_drop_down_set_selected(GTK_DROP_DOWN(app->rf_channel), channel);
	gtk_range_set_value(GTK_RANGE(app->move_speed), app->cfg[XY_CFG_OFF_MOVE_SPEED]);
	gtk_range_set_value(GTK_RANGE(app->scroll_speed), app->cfg[XY_CFG_OFF_SCROLL_SPEED]);
	for (guint i = 0; i < XY_FLAG_LAST; i++) {
		gtk_check_button_set_active(GTK_CHECK_BUTTON(app->flags[i]),
					    (app->cfg[xy_flags[i].offset] & xy_flags[i].mask) > 0);
	}
	xy_app_update_sensitivity(app);
}

static gboolean
xy_app_widgets_to_config(XyApp *app, guint8 *cfg, GError **error)
{
	const gchar *name = gtk_editable_get_text(GTK_EDITABLE(app->bt_name));
	gsize namesz = strlen(name);

	memcpy(cfg, app->cfg, XY_CFG_SIZE);
	if (namesz < 1) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
				    "The Bluetooth name is too short");
		return FALSE;
	}
	if (namesz > XY_CFG_BT_NAME_MAX) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			    "The Bluetooth name is too long, the maximum is %u bytes",
			    (guint)XY_CFG_BT_NAME_MAX);
		return FALSE;
	}
	/* like strcpy(), so keep whatever follows the NUL */
	memcpy(cfg + XY_CFG_OFF_BT_NAME, name, namesz + 1);

	xy_config_set_u16(cfg, XY_CFG_OFF_MAGIC, 0x3840);
	xy_config_set_u16(cfg,
			  XY_CFG_OFF_AUTO_OFF,
			  gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->auto_off)));
	xy_config_set_u16(cfg,
			  XY_CFG_OFF_TP_AUTO_OFF,
			  gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->tp_auto_off)));
	cfg[XY_CFG_OFF_RF_MODE] =
	    XY_RF_MODE_BT4 + gtk_drop_down_get_selected(GTK_DROP_DOWN(app->rf_mode));
	cfg[XY_CFG_OFF_RF_CHANNEL] = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->rf_channel));
	cfg[XY_CFG_OFF_MOVE_SPEED] = gtk_range_get_value(GTK_RANGE(app->move_speed));
	cfg[XY_CFG_OFF_SCROLL_SPEED] = gtk_range_get_value(GTK_RANGE(app->scroll_speed));
	for (guint i = 0; i < XY_FLAG_LAST; i++) {
		if (gtk_check_button_get_active(GTK_CHECK_BUTTON(app->flags[i])))
			cfg[xy_flags[i].offset] |= xy_flags[i].mask;
		else
			cfg[xy_flags[i].offset] &= ~xy_flags[i].mask;
	}
	xy_config_update_crc(cfg);
	return TRUE;
}

/* ---------------------------------------------------------------------------
 * background jobs
 */

static void
xy_job_progress_cb(guint percentage, gpointer user_data)
{
	XyApp *app = user_data;
	g_atomic_int_set(&app->progress_pct, percentage);
}

static gboolean
xy_app_progress_cb(gpointer user_data)
{
	XyApp *app = user_data;
	gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress),
				      g_atomic_int_get(&app->progress_pct) / 100.f);
	return G_SOURCE_CONTINUE;
}

typedef struct {
	XyApp *app;
	gchar *text;
} XyScanLine;

static gboolean
xy_app_scan_line_cb(gpointer user_data)
{
	XyScanLine *line = user_data;
	xy_app_log(line->app, "%s", line->text);
	g_free(line->text);
	g_free(line);
	return G_SOURCE_REMOVE;
}

static gboolean
xy_job_scancodes(XyJob *job, XyDevice *device, GCancellable *cancellable, GError **error)
{
	g_autofree gchar *last = NULL;

	if (!xy_device_scancode_start(device, error))
		return FALSE;
	while (!g_cancellable_is_cancelled(cancellable)) {
		g_autoptr(GError) error_local = NULL;
		XyScanLine *line;
		g_autofree gchar *str = xy_device_scancode_read(device, 250, &error_local);
		if (str == NULL) {
			if (g_error_matches(error_local, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
				continue;
			g_propagate_error(error, g_steal_pointer(&error_local));
			return FALSE;
		}
		/* the firmware streams reports continuously; only show changes */
		if (!g_set_str(&last, str))
			continue;
		line = g_new0(XyScanLine, 1);
		line->app = job->app;
		line->text = g_steal_pointer(&str);
		g_idle_add(xy_app_scan_line_cb, line);
	}
	return xy_device_scancode_stop(device, error);
}

static void
xy_job_thread_cb(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
	XyJob *job = task_data;
	XyApp *app = job->app;
	gboolean ret = FALSE;
	g_autoptr(GError) error = NULL;
	g_autoptr(XyDevice) device = NULL;

	device = xy_device_open_default(&error);
	if (device == NULL) {
		g_task_return_error(task, g_steal_pointer(&error));
		return;
	}
	switch (job->kind) {
	case XY_JOB_READ_CONFIG:
		ret = xy_device_read_config(device, job->cfg, xy_job_progress_cb, app, &error);
		break;
	case XY_JOB_WRITE_CONFIG:
		ret = xy_device_write_config(device, job->cfg, xy_job_progress_cb, app, &error);
		break;
	case XY_JOB_WRITE_LAYERS:
		ret = xy_device_write_layers(device, job->layers, xy_job_progress_cb, app, &error);
		break;
	case XY_JOB_WRITE_FIRMWARE:
		ret = xy_device_write_firmware(device,
					       job->fw,
					       job->fw_type,
					       xy_job_progress_cb,
					       app,
					       &error);
		break;
	case XY_JOB_RESTORE_CONFIG:
		ret = xy_device_restore_config(device, &error);
		break;
	case XY_JOB_ENTER_BOOT:
		ret = xy_device_enter_boot(device, job->bluetooth, &error);
		break;
	case XY_JOB_BACKLIGHT:
		ret = xy_device_set_backlight(device, job->percentage, &error);
		if (ret)
			job->result = g_strdup_printf("%u%%", job->percentage);
		break;
	case XY_JOB_VERSION:
		job->result = xy_device_get_version(device, &error);
		ret = job->result != NULL;
		break;
	case XY_JOB_BATTERY: {
		guint32 mv = 0;
		ret = xy_device_get_battery(device, &mv, &error);
		if (ret)
			job->result = g_strdup_printf("%u mV", mv);
		break;
	}
	case XY_JOB_SCANCODES:
		ret = xy_job_scancodes(job, device, cancellable, &error);
		break;
	}
	if (!ret) {
		if (error == NULL) {
			g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
				    "%s failed with no error set", job->title);
		}
		g_task_return_error(task, g_steal_pointer(&error));
		return;
	}
	g_task_return_boolean(task, TRUE);
}

static void
xy_app_set_busy(XyApp *app, gboolean busy)
{
	app->busy = busy;
	if (busy && app->progress_id == 0) {
		g_atomic_int_set(&app->progress_pct, 0);
		app->progress_id = g_timeout_add(100, xy_app_progress_cb, app);
	} else if (!busy && app->progress_id != 0) {
		g_source_remove(app->progress_id);
		app->progress_id = 0;
	}
	xy_app_update_sensitivity(app);
}

static void
xy_job_done_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
	XyJob *job = g_task_get_task_data(G_TASK(res));
	XyApp *app = job->app;
	g_autoptr(GError) error = NULL;

	if (job->kind == XY_JOB_SCANCODES) {
		g_clear_object(&app->scan_cancellable);
		gtk_button_set_label(GTK_BUTTON(app->scan_button), "Read Scan Codes");
	}
	xy_app_progress_cb(app);
	xy_app_set_busy(app, FALSE);

	if (!g_task_propagate_boolean(G_TASK(res), &error)) {
		xy_app_log(app, "%s failed: %s", job->title, error->message);
		xy_app_message(app, job->title, error->message);
		return;
	}
	gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress), 1.f);
	if (job->kind == XY_JOB_READ_CONFIG) {
		const gchar *board = xy_board_type_to_string(job->cfg[XY_CFG_OFF_BOARD]);
		memcpy(app->cfg, job->cfg, XY_CFG_SIZE);
		app->cfg_valid = TRUE;
		xy_app_config_to_widgets(app);
		xy_app_log(app,
			   "Read configuration from %s",
			   board != NULL ? board : "unknown board");
		return;
	}
	if (job->kind == XY_JOB_WRITE_CONFIG)
		memcpy(app->cfg, job->cfg, XY_CFG_SIZE);
	if (job->result != NULL) {
		xy_app_log(app, "%s: %s", job->title, job->result);
		if (job->kind != XY_JOB_BACKLIGHT)
			xy_app_message(app, job->title, job->result);
		return;
	}
	xy_app_log(app, "%s: success", job->title);
}

/* takes ownership of @job */
static void
xy_app_run_job(XyApp *app, XyJob *job, GCancellable *cancellable)
{
	g_autoptr(GTask) task = g_task_new(NULL, cancellable, xy_job_done_cb, NULL);
	g_task_set_check_cancellable(task, FALSE);
	g_task_set_task_data(task, job, (GDestroyNotify)xy_job_free);
	xy_app_set_busy(app, TRUE);
	xy_app_log(app, "%s…", job->title);
	g_task_run_in_thread(task, xy_job_thread_cb);
}

/* ---------------------------------------------------------------------------
 * file and confirmation dialogs
 */

typedef void (*XyFileFunc)(XyApp *app, GFile *file, gpointer user_data);

typedef struct {
	XyApp *app;
	XyFileFunc func;
	gpointer user_data;
	GDestroyNotify user_data_free;
	gboolean save;
} XyFileHelper;

static void
xy_file_helper_free(XyFileHelper *helper)
{
	if (helper->user_data_free != NULL && helper->user_data != NULL)
		helper->user_data_free(helper->user_data);
	g_free(helper);
}

/* @func takes ownership of user_data; it is only freed here on cancel */
static void
xy_file_helper_complete(XyFileHelper *helper, GFile *file)
{
	if (file != NULL)
		helper->func(helper->app, file, g_steal_pointer(&helper->user_data));
	xy_file_helper_free(helper);
}

static void
xy_file_dialog_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
	XyFileHelper *helper = user_data;
	g_autoptr(GFile) file = NULL;
	g_autoptr(GError) error = NULL;

	if (helper->save)
		file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), res, &error);
	else
		file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), res, &error);
	xy_file_helper_complete(helper, file);
}

static void
xy_app_choose_file(XyApp *app,
		   const gchar *title,
		   const gchar *pattern,
		   const gchar *filter_name,
		   const gchar *initial_name,
		   gboolean save,
		   XyFileFunc func,
		   gpointer user_data,
		   GDestroyNotify user_data_free)
{
	XyFileHelper *helper = g_new0(XyFileHelper, 1);
	g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
	g_autoptr(GtkFileFilter) filter = gtk_file_filter_new();
	g_autoptr(GListStore) filters = g_list_store_new(GTK_TYPE_FILE_FILTER);

	helper->app = app;
	helper->func = func;
	helper->user_data = user_data;
	helper->user_data_free = user_data_free;
	helper->save = save;

	gtk_file_dialog_set_title(dialog, title);
	gtk_file_dialog_set_modal(dialog, TRUE);
	gtk_file_filter_set_name(filter, filter_name);
	gtk_file_filter_add_pattern(filter, pattern);
	g_list_store_append(filters, filter);
	gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
	gtk_file_dialog_set_default_filter(dialog, filter);
	if (initial_name != NULL)
		gtk_file_dialog_set_initial_name(dialog, initial_name);
	if (save)
		gtk_file_dialog_save(dialog, app->window, NULL, xy_file_dialog_cb, helper);
	else
		gtk_file_dialog_open(dialog, app->window, NULL, xy_file_dialog_cb, helper);
}

typedef struct {
	XyApp *app;
	XyJob *job;
} XyConfirmHelper;

static void
xy_confirm_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
	XyConfirmHelper *helper = user_data;
	gint button = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), res, NULL);

	if (button == 1)
		xy_app_run_job(helper->app, helper->job, NULL);
	else
		xy_job_free(helper->job);
	g_free(helper);
}

/* takes ownership of @job */
static void
xy_app_confirm_job(XyApp *app, XyJob *job, const gchar *message, const gchar *detail)
{
	const gchar *buttons[] = {"Cancel", "Continue", NULL};
	XyConfirmHelper *helper = g_new0(XyConfirmHelper, 1);
	g_autoptr(GtkAlertDialog) dialog = gtk_alert_dialog_new("%s", message);

	helper->app = app;
	helper->job = job;
	gtk_alert_dialog_set_detail(dialog, detail);
	gtk_alert_dialog_set_buttons(dialog, buttons);
	gtk_alert_dialog_set_cancel_button(dialog, 0);
	gtk_alert_dialog_set_default_button(dialog, 0);
	gtk_alert_dialog_choose(dialog, app->window, NULL, xy_confirm_cb, helper);
}

/* ---------------------------------------------------------------------------
 * button handlers
 */

typedef struct {
	const gchar *title;
	const gchar *filename;
	const gchar *detail;
	guint16 fw_type;
} XyFirmwareInfo;

static const XyFirmwareInfo xy_fw_boot = {
    "Upgrade BOOT",
    "xykbms_bootupdate_release.bin",
    "After upgrading BOOT the APP firmware must be downloaded again.",
    XY_FIRMWARE_TYPE_APP,
};
static const XyFirmwareInfo xy_fw_app = {
    "Upgrade APP",
    "xykbms_app_release.bin",
    "After upgrading APP the pin configuration file must be downloaded again.",
    XY_FIRMWARE_TYPE_APP,
};
static const XyFirmwareInfo xy_fw_usb = {
    "Upgrade USB",
    "xykbms_usb_release.bin",
    "This is only for boards with USB. After upgrading, the pin configuration file "
    "must be downloaded again.",
    XY_FIRMWARE_TYPE_USB,
};

static void
xy_app_firmware_file_cb(XyApp *app, GFile *file, gpointer user_data)
{
	const XyFirmwareInfo *info = user_data;
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) fw = g_file_load_bytes(file, NULL, NULL, &error);
	g_autofree gchar *basename = g_file_get_basename(file);
	g_autofree gchar *message = NULL;
	XyJob *job;

	if (fw == NULL) {
		xy_app_message(app, info->title, error->message);
		return;
	}
	job = xy_job_new(app, XY_JOB_WRITE_FIRMWARE, info->title);
	job->fw = g_bytes_ref(fw);
	job->fw_type = info->fw_type;
	message = g_strdup_printf("%s with %s?", info->title, basename);
	xy_app_confirm_job(app, job, message, info->detail);
}

static void
xy_app_firmware_clicked_cb(GtkButton *button, gpointer user_data)
{
	XyApp *app = g_object_get_data(G_OBJECT(button), "app");
	const XyFirmwareInfo *info = user_data;
	g_autofree gchar *title = g_strdup_printf("Select %s", info->filename);

	xy_app_choose_file(app,
			   title,
			   "*.bin",
			   "Firmware images (*.bin)",
			   NULL,
			   FALSE,
			   xy_app_firmware_file_cb,
			   (gpointer)info,
			   NULL);
}

static void
xy_app_restore_clicked_cb(GtkButton *button, XyApp *app)
{
	xy_app_confirm_job(app,
			   xy_job_new(app, XY_JOB_RESTORE_CONFIG, "Restore Config"),
			   "Restore the configuration?",
			   "This clears all pairing information and user settings. Afterwards "
			   "the keyboard must be paired again and the pin configuration file "
			   "downloaded again.");
}

static void
xy_app_read_config_clicked_cb(GtkButton *button, XyApp *app)
{
	xy_app_run_job(app, xy_job_new(app, XY_JOB_READ_CONFIG, "Read Config"), NULL);
}

static void
xy_app_pin_file_cb(XyApp *app, GFile *file, gpointer user_data)
{
	g_autoptr(XyJob) job = user_data;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = g_file_get_path(file);

	if (!xy_config_apply_pin_file(job->cfg, path, &error)) {
		xy_app_message(app, "Failed to load the pin configuration file", error->message);
		return;
	}
	xy_app_log(app, "Loaded pin configuration %s", path);
	xy_app_run_job(app, g_steal_pointer(&job), NULL);
}

static void
xy_app_write_config_clicked_cb(GtkButton *button, XyApp *app)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(XyJob) job = NULL;

	if (!app->cfg_valid) {
		xy_app_message(app,
			       "Download Config",
			       "Please read the configuration first, modify it, then download it.");
		return;
	}
	job = xy_job_new(app, XY_JOB_WRITE_CONFIG, "Download Config");
	if (!xy_app_widgets_to_config(app, job->cfg, &error)) {
		xy_app_message(app, "Download Config", error->message);
		return;
	}
	if (gtk_check_button_get_active(GTK_CHECK_BUTTON(app->update_pin_cfg))) {
		xy_app_choose_file(app,
				   "Select XYKBMS pin configuration file",
				   "*.cfg",
				   "XYKBMS pin configuration (*.cfg)",
				   NULL,
				   FALSE,
				   xy_app_pin_file_cb,
				   g_steal_pointer(&job),
				   (GDestroyNotify)xy_job_free);
		return;
	}
	xy_app_run_job(app, g_steal_pointer(&job), NULL);
}

static void
xy_app_layers_file_cb(XyApp *app, GFile *file, gpointer user_data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = g_file_get_path(file);
	g_autoptr(XyJob) job = xy_job_new(app, XY_JOB_WRITE_LAYERS, "Download Layers");

	job->layers = g_malloc(XY_LAYERS_SIZE);
	if (!xy_layers_load(job->layers, path, &error)) {
		xy_app_message(app, "Failed to load the layer configuration file", error->message);
		return;
	}
	xy_app_run_job(app, g_steal_pointer(&job), NULL);
}

static void
xy_app_write_layers_clicked_cb(GtkButton *button, XyApp *app)
{
	xy_app_choose_file(app,
			   "Select XYKBMS layer configuration file",
			   "*.cfg",
			   "XYKBMS layer configuration (*.cfg)",
			   NULL,
			   FALSE,
			   xy_app_layers_file_cb,
			   NULL,
			   NULL);
}

static void
xy_app_enter_boot_clicked_cb(GtkButton *button, XyApp *app)
{
	XyJob *job = xy_job_new(app, XY_JOB_ENTER_BOOT, "Enter BOOT");
	job->bluetooth = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "bluetooth"));
	xy_app_run_job(app, job, NULL);
}

static void
xy_app_save_file_cb(XyApp *app, GFile *file, gpointer user_data)
{
	g_autofree guint8 *cfg = user_data;
	g_autoptr(GError) error = NULL;

	if (!g_file_replace_contents(file,
				     (const gchar *)cfg,
				     XY_CFG_SIZE,
				     NULL,
				     FALSE,
				     G_FILE_CREATE_NONE,
				     NULL,
				     NULL,
				     &error)) {
		xy_app_message(app, "Failed to save the configuration", error->message);
		return;
	}
	xy_app_log(app, "Saved configuration to %s", g_file_peek_path(file));
}

static void
xy_app_save_clicked_cb(GtkButton *button, XyApp *app)
{
	g_autoptr(GError) error = NULL;
	g_autofree guint8 *cfg = g_malloc(XY_CFG_SIZE);

	if (!app->cfg_valid) {
		xy_app_message(app, "Save Config", "Please read the configuration first.");
		return;
	}
	if (!xy_app_widgets_to_config(app, cfg, &error)) {
		xy_app_message(app, "Save Config", error->message);
		return;
	}
	xy_app_choose_file(app,
			   "Save configuration",
			   "*.bin",
			   "XYKBMS configuration (*.bin)",
			   "xykbms_config.bin",
			   TRUE,
			   xy_app_save_file_cb,
			   g_steal_pointer(&cfg),
			   g_free);
}

static void
xy_app_open_file_cb(XyApp *app, GFile *file, gpointer user_data)
{
	gsize bufsz = 0;
	g_autofree gchar *buf = NULL;
	g_autoptr(GError) error = NULL;

	if (!g_file_load_contents(file, NULL, &buf, &bufsz, NULL, &error) ||
	    !xy_config_validate((const guint8 *)buf, bufsz, &error)) {
		xy_app_message(app, "Failed to open the configuration", error->message);
		return;
	}
	memcpy(app->cfg, buf, XY_CFG_SIZE);
	app->cfg_valid = TRUE;
	xy_app_config_to_widgets(app);
	xy_app_log(app, "Loaded configuration from %s", g_file_peek_path(file));
}

static void
xy_app_open_clicked_cb(GtkButton *button, XyApp *app)
{
	xy_app_choose_file(app,
			   "Open configuration",
			   "*.bin",
			   "XYKBMS configuration (*.bin)",
			   NULL,
			   FALSE,
			   xy_app_open_file_cb,
			   NULL,
			   NULL);
}

static void
xy_app_version_clicked_cb(GtkButton *button, XyApp *app)
{
	xy_app_run_job(app, xy_job_new(app, XY_JOB_VERSION, "Firmware Version"), NULL);
}

static void
xy_app_battery_clicked_cb(GtkButton *button, XyApp *app)
{
	xy_app_run_job(app, xy_job_new(app, XY_JOB_BATTERY, "Battery Voltage"), NULL);
}

static void
xy_app_scan_clicked_cb(GtkButton *button, XyApp *app)
{
	if (app->scan_cancellable != NULL) {
		g_cancellable_cancel(app->scan_cancellable);
		return;
	}
	app->scan_cancellable = g_cancellable_new();
	gtk_button_set_label(button, "Stop Reading");
	xy_app_run_job(app,
		       xy_job_new(app, XY_JOB_SCANCODES, "Read Scan Codes"),
		       app->scan_cancellable);
	xy_app_log(app, "Press keys on the keyboard; the first value is the scan code");
}

static gboolean
xy_app_backlight_timeout_cb(gpointer user_data)
{
	XyApp *app = user_data;
	XyJob *job = xy_job_new(app, XY_JOB_BACKLIGHT, "Backlight");

	app->backlight_id = 0;
	job->percentage = gtk_range_get_value(GTK_RANGE(app->backlight));
	if (app->busy) {
		xy_job_free(job);
		return G_SOURCE_REMOVE;
	}
	xy_app_run_job(app, job, NULL);
	return G_SOURCE_REMOVE;
}

static void
xy_app_backlight_changed_cb(GtkRange *range, XyApp *app)
{
	/* the vendor tool only sends the value when the slider is released */
	if (app->backlight_id != 0)
		g_source_remove(app->backlight_id);
	app->backlight_id = g_timeout_add(400, xy_app_backlight_timeout_cb, app);
}

static void
xy_app_sta_toggled_cb(GtkCheckButton *button, XyApp *app)
{
	xy_app_update_sensitivity(app);
}

static const gchar *xy_help_text =
    "UPGRADING FIRMWARE\n"
    "\n"
    "Upgrade order: BOOT (only if needed) → APP → pair again and download the "
    "configuration.\n"
    "\n"
    "Firmware can only be upgraded while the board is in BOOT mode. To enter BOOT:\n"
    "  1. If the keyboard works normally, press “Enter BOOT”.\n"
    "  2. Otherwise force it: first-generation boards, hold the blue key while "
    "switching on; NumLock and CapsLock flash alternately in BOOT. "
    "Second-generation boards enter BOOT directly when the battery is connected. "
    "The board stays in BOOT for only about 10 seconds.\n"
    "\n"
    "BOOT upgrade:\n"
    "  1. Press “Upgrade BOOT” and wait. If the progress bar stops for a long "
    "time the transfer was disturbed; enter BOOT again and retry.\n"
    "  2. After a BOOT upgrade the board enters BOOT on every power-up until APP "
    "is upgraded.\n"
    "\n"
    "APP upgrade:\n"
    "  1. Press “Upgrade APP” and wait.\n"
    "  2. Press “Read Config”, tick “Update pin configuration file” and change "
    "other options as needed.\n"
    "  3. Press “Download Config” and choose the .cfg for your keyboard model.\n"
    "\n"
    "Restore config (in BOOT): clears all pairing information and user settings. "
    "Afterwards pair again and download the pin configuration.\n"
    "\n"
    "Tip: keep the keyboard close to the receiver while upgrading.\n"
    "\n"
    "CONFIGURING THE KEYBOARD\n"
    "\n"
    "  1. Connect the keyboard.\n"
    "  2. Press “Read Config”.\n"
    "  3. Change the options. After an APP upgrade tick “Update pin configuration "
    "file”.\n"
    "  4. Press “Download Config”.\n"
    "\n"
    "Custom key maps: press “Read Scan Codes” and press each key to learn its scan "
    "code (the first value), then put the HID usage ID at that position in the key "
    "table of a copy of a .cfg file. See the comments inside the .cfg files and "
    "the USB HID Usage Tables.\n"
    "\n"
    "Quick configuration without the PC tool: press Insert six times to enter "
    "configuration mode, choose a function with Fx or a letter, then a value with "
    "a number key.\n";

static void
xy_app_help_clicked_cb(GtkButton *button, XyApp *app)
{
	GtkWidget *window = gtk_window_new();
	GtkWidget *scrolled = gtk_scrolled_window_new();
	GtkWidget *view = gtk_text_view_new();

	gtk_window_set_title(GTK_WINDOW(window), "Help");
	gtk_window_set_transient_for(GTK_WINDOW(window), app->window);
	gtk_window_set_default_size(GTK_WINDOW(window), 560, 600);
	gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD);
	gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 12);
	gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view), 12);
	gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view), 12);
	gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)), xy_help_text, -1);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), view);
	gtk_window_set_child(GTK_WINDOW(window), scrolled);
	gtk_window_present(GTK_WINDOW(window));
}

/* ---------------------------------------------------------------------------
 * window construction
 */

static GtkWidget *
xy_app_add_button(XyApp *app, GtkWidget *box, const gchar *label, const gchar *tooltip,
		  GCallback cb, gpointer user_data)
{
	GtkWidget *button = gtk_button_new_with_label(label);
	if (tooltip != NULL)
		gtk_widget_set_tooltip_text(button, tooltip);
	g_object_set_data(G_OBJECT(button), "app", app);
	g_signal_connect(button, "clicked", cb, user_data);
	gtk_widget_set_hexpand(button, TRUE);
	gtk_box_append(GTK_BOX(box), button);
	g_ptr_array_add(app->action_buttons, button);
	return button;
}

static GtkWidget *
xy_app_frame(const gchar *title, GtkWidget **inner)
{
	GtkWidget *frame = gtk_frame_new(title);
	*inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_set_margin_start(*inner, 8);
	gtk_widget_set_margin_end(*inner, 8);
	gtk_widget_set_margin_top(*inner, 8);
	gtk_widget_set_margin_bottom(*inner, 8);
	gtk_frame_set_child(GTK_FRAME(frame), *inner);
	return frame;
}

static GtkWidget *
xy_app_row(void)
{
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_box_set_homogeneous(GTK_BOX(row), TRUE);
	return row;
}

static void
xy_grid_attach_label(GtkWidget *grid, const gchar *text, gint row)
{
	GtkWidget *label = gtk_label_new(text);
	gtk_label_set_xalign(GTK_LABEL(label), 0.f);
	gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
}

static GtkWidget *
xy_app_build_options(XyApp *app)
{
	GtkWidget *inner;
	GtkWidget *frame = xy_app_frame("Configuration Options", &inner);
	GtkWidget *grid = gtk_grid_new();
	GtkWidget *flags_grid = gtk_grid_new();
	GtkWidget *rf_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gint row = 0;

	gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 12);

	xy_grid_attach_label(grid, "Board", row);
	app->board_label = gtk_label_new("Read the configuration to begin");
	gtk_label_set_xalign(GTK_LABEL(app->board_label), 0.f);
	gtk_widget_add_css_class(app->board_label, "dim-label");
	gtk_grid_attach(GTK_GRID(grid), app->board_label, 1, row++, 1, 1);

	xy_grid_attach_label(grid, "Bluetooth device name", row);
	app->bt_name = gtk_entry_new();
	gtk_entry_set_max_length(GTK_ENTRY(app->bt_name), XY_CFG_BT_NAME_MAX);
	gtk_widget_set_hexpand(app->bt_name, TRUE);
	gtk_grid_attach(GTK_GRID(grid), app->bt_name, 1, row++, 1, 1);

	xy_grid_attach_label(grid, "Keyboard auto-off time (minutes)", row);
	app->auto_off = gtk_spin_button_new_with_range(0, G_MAXUINT16, 1);
	gtk_grid_attach(GTK_GRID(grid), app->auto_off, 1, row++, 1, 1);

	xy_grid_attach_label(grid, "TrackPoint auto-off time (minutes)", row);
	app->tp_auto_off = gtk_spin_button_new_with_range(0, G_MAXUINT16, 1);
	gtk_grid_attach(GTK_GRID(grid), app->tp_auto_off, 1, row++, 1, 1);

	xy_grid_attach_label(grid, "Default wireless connection", row);
	app->rf_mode = gtk_drop_down_new_from_strings(xy_rf_modes);
	app->rf_channel = gtk_drop_down_new_from_strings(xy_rf_channels);
	gtk_widget_set_tooltip_text(app->rf_channel, "Channel");
	gtk_box_append(GTK_BOX(rf_box), app->rf_mode);
	gtk_box_append(GTK_BOX(rf_box), app->rf_channel);
	gtk_grid_attach(GTK_GRID(grid), rf_box, 1, row++, 1, 1);

	xy_grid_attach_label(grid, "TrackPoint speed", row);
	app->move_speed = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0x50, 0xff, 1);
	gtk_scale_set_draw_value(GTK_SCALE(app->move_speed), TRUE);
	gtk_scale_set_digits(GTK_SCALE(app->move_speed), 0);
	gtk_grid_attach(GTK_GRID(grid), app->move_speed, 1, row++, 1, 1);

	xy_grid_attach_label(grid, "Scroll speed", row);
	app->scroll_speed = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1, 15, 1);
	gtk_scale_set_draw_value(GTK_SCALE(app->scroll_speed), TRUE);
	gtk_scale_set_digits(GTK_SCALE(app->scroll_speed), 0);
	gtk_grid_attach(GTK_GRID(grid), app->scroll_speed, 1, row++, 1, 1);

	gtk_box_append(GTK_BOX(inner), grid);

	gtk_grid_set_row_spacing(GTK_GRID(flags_grid), 2);
	gtk_grid_set_column_spacing(GTK_GRID(flags_grid), 12);
	gtk_widget_set_margin_top(flags_grid, 8);
	for (guint i = 0; i < XY_FLAG_LAST; i++) {
		app->flags[i] = gtk_check_button_new_with_label(xy_flags[i].label);
		if (xy_flags[i].tooltip != NULL)
			gtk_widget_set_tooltip_text(app->flags[i], xy_flags[i].tooltip);
		gtk_grid_attach(GTK_GRID(flags_grid), app->flags[i], i % 2, i / 2, 1, 1);
	}
	g_signal_connect(app->flags[XY_FLAG_DISABLE_STA_BLINK],
			 "toggled",
			 G_CALLBACK(xy_app_sta_toggled_cb),
			 app);
	gtk_box_append(GTK_BOX(inner), flags_grid);

	app->options_box = inner;
	return frame;
}

static GtkWidget *
xy_app_build_actions(XyApp *app)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	GtkWidget *inner;
	GtkWidget *row;
	GtkWidget *frame;
	GtkWidget *bl_box;
	GtkWidget *label;
	GtkWidget *button;
	GtkWidget *scrolled;

	/* BOOT-only */
	frame = xy_app_frame("Firmware — use only in BOOT mode, with care!", &inner);
	row = xy_app_row();
	xy_app_add_button(app, row, "Upgrade BOOT", NULL,
			  G_CALLBACK(xy_app_firmware_clicked_cb), (gpointer)&xy_fw_boot);
	xy_app_add_button(app, row, "Upgrade APP", NULL,
			  G_CALLBACK(xy_app_firmware_clicked_cb), (gpointer)&xy_fw_app);
	xy_app_add_button(app, row, "Upgrade USB", "Only for boards with USB",
			  G_CALLBACK(xy_app_firmware_clicked_cb), (gpointer)&xy_fw_usb);
	xy_app_add_button(app, row, "Restore Config",
			  "Clear all pairing information and user settings",
			  G_CALLBACK(xy_app_restore_clicked_cb), app);
	gtk_box_append(GTK_BOX(inner), row);
	gtk_box_append(GTK_BOX(box), frame);

	/* config */
	frame = xy_app_frame("Keyboard", &inner);
	row = xy_app_row();
	xy_app_add_button(app, row, "Read Config", NULL,
			  G_CALLBACK(xy_app_read_config_clicked_cb), app);
	xy_app_add_button(app, row, "Download Config", NULL,
			  G_CALLBACK(xy_app_write_config_clicked_cb), app);
	xy_app_add_button(app, row, "Download Layers", "Write a layer configuration file",
			  G_CALLBACK(xy_app_write_layers_clicked_cb), app);
	button = xy_app_add_button(app, row, "Enter BOOT", "Reboot the keyboard into BOOT mode",
				   G_CALLBACK(xy_app_enter_boot_clicked_cb), app);
	g_object_set_data(G_OBJECT(button), "bluetooth", GINT_TO_POINTER(FALSE));
	gtk_box_append(GTK_BOX(inner), row);

	app->update_pin_cfg = gtk_check_button_new_with_label(
	    "Update pin configuration file when downloading config");
	gtk_widget_set_tooltip_text(app->update_pin_cfg,
				    "Needed after an APP upgrade; choose the .cfg for your keyboard");
	gtk_box_append(GTK_BOX(inner), app->update_pin_cfg);

	row = xy_app_row();
	xy_app_add_button(app, row, "Open Config File…", "Load a saved xykbms_config.bin",
			  G_CALLBACK(xy_app_open_clicked_cb), app);
	xy_app_add_button(app, row, "Save Config File…", "Save the configuration to a file",
			  G_CALLBACK(xy_app_save_clicked_cb), app);
	gtk_box_append(GTK_BOX(inner), row);

	bl_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	label = gtk_label_new("Backlight");
	app->backlight = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
	gtk_scale_set_digits(GTK_SCALE(app->backlight), 0);
	gtk_scale_set_draw_value(GTK_SCALE(app->backlight), TRUE);
	gtk_widget_set_hexpand(app->backlight, TRUE);
	g_signal_connect(app->backlight, "value-changed",
			 G_CALLBACK(xy_app_backlight_changed_cb), app);
	gtk_box_append(GTK_BOX(bl_box), label);
	gtk_box_append(GTK_BOX(bl_box), app->backlight);
	gtk_box_append(GTK_BOX(inner), bl_box);
	gtk_box_append(GTK_BOX(box), frame);

	/* tools */
	frame = xy_app_frame("Tools", &inner);
	row = xy_app_row();
	xy_app_add_button(app, row, "Battery Voltage", NULL,
			  G_CALLBACK(xy_app_battery_clicked_cb), app);
	xy_app_add_button(app, row, "Read Version", NULL,
			  G_CALLBACK(xy_app_version_clicked_cb), app);
	app->scan_button = gtk_button_new_with_label("Read Scan Codes");
	gtk_widget_set_hexpand(app->scan_button, TRUE);
	g_signal_connect(app->scan_button, "clicked", G_CALLBACK(xy_app_scan_clicked_cb), app);
	gtk_box_append(GTK_BOX(row), app->scan_button);
	gtk_box_append(GTK_BOX(inner), row);
	gtk_box_append(GTK_BOX(box), frame);

	app->progress = gtk_progress_bar_new();
	gtk_box_append(GTK_BOX(box), app->progress);

	app->log_view = gtk_text_view_new();
	gtk_text_view_set_editable(GTK_TEXT_VIEW(app->log_view), FALSE);
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(app->log_view), TRUE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(app->log_view), GTK_WRAP_WORD_CHAR);
	scrolled = gtk_scrolled_window_new();
	gtk_widget_set_vexpand(scrolled, TRUE);
	gtk_widget_set_size_request(scrolled, -1, 160);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), app->log_view);
	frame = gtk_frame_new(NULL);
	gtk_frame_set_child(GTK_FRAME(frame), scrolled);
	gtk_box_append(GTK_BOX(box), frame);

	return box;
}

static void
xy_app_bt_boot_activate_cb(GSimpleAction *action, GVariant *param, gpointer user_data)
{
	XyApp *app = user_data;
	XyJob *job = xy_job_new(app, XY_JOB_ENTER_BOOT, "Enter BOOT (Bluetooth)");
	if (app->busy) {
		xy_job_free(job);
		return;
	}
	job->bluetooth = TRUE;
	xy_app_run_job(app, job, NULL);
}

static void
xy_app_activate_cb(GApplication *application, XyApp *app)
{
	GtkWidget *header;
	GtkWidget *help;
	GtkWidget *menu_button;
	GtkWidget *paned;
	g_autoptr(GMenu) menu = g_menu_new();
	g_autoptr(GSimpleAction) bt_boot = g_simple_action_new("enter-boot-bt", NULL);
	g_autoptr(GPtrArray) paths = NULL;
	g_autoptr(GError) error = NULL;

	if (app->window != NULL) {
		gtk_window_present(app->window);
		return;
	}
	app->window = GTK_WINDOW(gtk_application_window_new(app->application));
	gtk_window_set_title(app->window, "XYKBMS Keyboard Configuration");
	gtk_window_set_default_size(app->window, 1100, 640);

	g_signal_connect(bt_boot, "activate", G_CALLBACK(xy_app_bt_boot_activate_cb), app);
	g_action_map_add_action(G_ACTION_MAP(app->window), G_ACTION(bt_boot));
	g_menu_append(menu, "Enter BOOT (Bluetooth)", "win.enter-boot-bt");

	header = gtk_header_bar_new();
	help = gtk_button_new_with_label("Help");
	g_signal_connect(help, "clicked", G_CALLBACK(xy_app_help_clicked_cb), app);
	gtk_header_bar_pack_start(GTK_HEADER_BAR(header), help);
	menu_button = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button), "open-menu-symbolic");
	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button), G_MENU_MODEL(menu));
	gtk_header_bar_pack_end(GTK_HEADER_BAR(header), menu_button);
	gtk_window_set_titlebar(app->window, header);

	paned = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_widget_set_margin_start(paned, 12);
	gtk_widget_set_margin_end(paned, 12);
	gtk_widget_set_margin_top(paned, 12);
	gtk_widget_set_margin_bottom(paned, 12);
	gtk_box_append(GTK_BOX(paned), xy_app_build_actions(app));
	gtk_box_append(GTK_BOX(paned), xy_app_build_options(app));
	gtk_window_set_child(app->window, paned);

	xy_app_update_sensitivity(app);
	gtk_window_present(app->window);

	/* say what we found */
	paths = xy_device_enumerate(&error);
	if (paths == NULL) {
		xy_app_log(app, "Failed to enumerate devices: %s", error->message);
	} else if (paths->len == 0) {
		xy_app_log(app, "No keyboard detected — plug in the receiver or keyboard");
	} else {
		for (guint i = 0; i < paths->len; i++) {
			const gchar *path = g_ptr_array_index(paths, i);
			if (g_access(path, R_OK | W_OK) != 0)
				xy_app_log(app,
					   "Found %s but cannot open it; install 70-xykbms.rules",
					   path);
			else
				xy_app_log(app, "Found keyboard at %s", path);
		}
	}
}

int
main(int argc, char **argv)
{
	XyApp app = {0};
	gint rc;

	app.action_buttons = g_ptr_array_new();
	app.application = gtk_application_new("io.github.hughsie.xykbms-gtk",
					      G_APPLICATION_DEFAULT_FLAGS);
	g_signal_connect(app.application, "activate", G_CALLBACK(xy_app_activate_cb), &app);
	rc = g_application_run(G_APPLICATION(app.application), argc, argv);
	if (app.scan_cancellable != NULL)
		g_cancellable_cancel(app.scan_cancellable);
	g_object_unref(app.application);
	g_ptr_array_unref(app.action_buttons);
	return rc;
}
