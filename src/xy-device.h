/*
 * Copyright 2026 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <gio/gio.h>

#define XY_USB_VID 0x5859

/* firmware image types, OR'd into the high byte of the block index */
#define XY_FIRMWARE_TYPE_APP 0x0000
#define XY_FIRMWARE_TYPE_USB 0x4000

typedef struct _XyDevice XyDevice;

/* called from the worker thread */
typedef void (*XyProgressFunc)(guint percentage, gpointer user_data);

GPtrArray *
xy_device_enumerate(GError **error);
XyDevice *
xy_device_open(const gchar *path, GError **error);
XyDevice *
xy_device_open_default(GError **error);
XyDevice *
xy_device_new_for_fd(gint fd);
void
xy_device_free(XyDevice *self);
const gchar *
xy_device_get_path(XyDevice *self);

gboolean
xy_device_read_config(XyDevice *self,
		      guint8 *cfg,
		      XyProgressFunc progress_cb,
		      gpointer user_data,
		      GError **error);
gboolean
xy_device_write_config(XyDevice *self,
		       const guint8 *cfg,
		       XyProgressFunc progress_cb,
		       gpointer user_data,
		       GError **error);
gboolean
xy_device_write_layers(XyDevice *self,
		       const guint8 *layers,
		       XyProgressFunc progress_cb,
		       gpointer user_data,
		       GError **error);
gboolean
xy_device_write_firmware(XyDevice *self,
			 GBytes *fw,
			 guint16 fw_type,
			 XyProgressFunc progress_cb,
			 gpointer user_data,
			 GError **error);
gboolean
xy_device_restore_config(XyDevice *self, GError **error);
gboolean
xy_device_enter_boot(XyDevice *self, gboolean bluetooth, GError **error);
gboolean
xy_device_set_backlight(XyDevice *self, guint8 percentage, GError **error);
gchar *
xy_device_get_version(XyDevice *self, GError **error);
gboolean
xy_device_get_battery(XyDevice *self, guint32 *millivolts, GError **error);

gboolean
xy_device_scancode_start(XyDevice *self, GError **error);
gboolean
xy_device_scancode_stop(XyDevice *self, GError **error);
gchar *
xy_device_scancode_read(XyDevice *self, guint timeout_ms, GError **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(XyDevice, xy_device_free)
