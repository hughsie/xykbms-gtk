/*
 * Copyright 2026 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <errno.h>
#include <glib-unix.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "xy-config.h"
#include "xy-device.h"

#define XY_REPORT_ID	   0x02
#define XY_PACKET_SIZE	   19 /* report ID + 18 bytes */
#define XY_TIMEOUT_MS	   2000
#define XY_RETRIES_CMD	   3
#define XY_RETRIES_BULK	   6
#define XY_FILL_BYTE	   0xfd
#define XY_CMD_KIND_CTRL   0x3f
#define XY_CMD_KIND_DATA   0x3e
#define XY_BLOCK_SIZE	   16
#define XY_READ_BLOCK_SIZE 8
#define XY_FW_MAX_SIZE	   0x20000
#define XY_FW_ALIGN	   0x800

#define XY_CTRL_READ_CONFIG  0x22
#define XY_CTRL_WRITE_CONFIG 0x11
#define XY_CTRL_WRITE_LAYERS 0x33

#define XY_DATA_ENTER_BOOT    0xcc
#define XY_DATA_ENTER_BOOT_BT 0xcd
#define XY_DATA_SCAN_START    0x44
#define XY_DATA_SCAN_STOP     0x45
#define XY_DATA_BACKLIGHT     0x66
#define XY_DATA_BATTERY	      0x6a
#define XY_DATA_VERSION	      0x6b

struct _XyDevice {
	gchar *path;
	gint fd;
};

static gboolean
xy_device_report_descriptor_is_vendor(const guint8 *buf, gsize bufsz)
{
	gboolean has_vendor_page = FALSE;
	gboolean has_report_id = FALSE;

	/* Usage Page (0xFF00) and Report ID (2) */
	for (gsize i = 0; i + 2 < bufsz; i++) {
		if (buf[i] == 0x06 && buf[i + 1] == 0x00 && buf[i + 2] == 0xff)
			has_vendor_page = TRUE;
	}
	for (gsize i = 0; i + 1 < bufsz; i++) {
		if (buf[i] == 0x85 && buf[i + 1] == XY_REPORT_ID)
			has_report_id = TRUE;
	}
	return has_vendor_page && has_report_id;
}

static gboolean
xy_device_sysfs_matches(const gchar *sysfs_dir)
{
	guint vid = 0;
	gsize rdescsz = 0;
	g_autofree gchar *uevent = NULL;
	g_autofree gchar *uevent_fn = g_build_filename(sysfs_dir, "device", "uevent", NULL);
	g_autofree gchar *rdesc = NULL;
	g_autofree gchar *rdesc_fn =
	    g_build_filename(sysfs_dir, "device", "report_descriptor", NULL);
	g_auto(GStrv) lines = NULL;

	if (!g_file_get_contents(uevent_fn, &uevent, NULL, NULL))
		return FALSE;
	lines = g_strsplit(uevent, "\n", -1);
	for (guint i = 0; lines[i] != NULL; i++) {
		guint bus = 0;
		guint pid = 0;
		if (sscanf(lines[i], "HID_ID=%x:%x:%x", &bus, &vid, &pid) == 3)
			break;
	}
	if (vid != XY_USB_VID)
		return FALSE;
	if (!g_file_get_contents(rdesc_fn, &rdesc, &rdescsz, NULL))
		return FALSE;
	return xy_device_report_descriptor_is_vendor((const guint8 *)rdesc, rdescsz);
}

GPtrArray *
xy_device_enumerate(GError **error)
{
	const gchar *name;
	g_autoptr(GDir) dir = NULL;
	g_autoptr(GPtrArray) paths = g_ptr_array_new_with_free_func(g_free);

	dir = g_dir_open("/sys/class/hidraw", 0, error);
	if (dir == NULL)
		return NULL;
	while ((name = g_dir_read_name(dir)) != NULL) {
		g_autofree gchar *sysfs_dir = g_build_filename("/sys/class/hidraw", name, NULL);
		if (xy_device_sysfs_matches(sysfs_dir))
			g_ptr_array_add(paths, g_build_filename("/dev", name, NULL));
	}
	return g_steal_pointer(&paths);
}

XyDevice *
xy_device_open(const gchar *path, GError **error)
{
	g_autoptr(XyDevice) self = g_new0(XyDevice, 1);

	self->path = g_strdup(path);
	self->fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (self->fd < 0) {
		gint errsv = errno;
		g_set_error(error,
			    G_IO_ERROR,
			    g_io_error_from_errno(errsv),
			    "failed to open %s: %s%s",
			    path,
			    g_strerror(errsv),
			    errsv == EACCES ? " (is the udev rule installed?)" : "");
		return NULL;
	}
	return g_steal_pointer(&self);
}

/* takes ownership of @fd; used by the self tests with a socketpair */
XyDevice *
xy_device_new_for_fd(gint fd)
{
	XyDevice *self = g_new0(XyDevice, 1);
	self->path = g_strdup_printf("fd:%i", fd);
	self->fd = fd;
	if (fd >= 0)
		g_unix_set_fd_nonblocking(fd, TRUE, NULL);
	return self;
}

XyDevice *
xy_device_open_default(GError **error)
{
	const gchar *path = g_getenv("XYKBMS_HIDRAW");
	g_autoptr(GPtrArray) paths = NULL;

	if (path != NULL)
		return xy_device_open(path, error);
	paths = xy_device_enumerate(error);
	if (paths == NULL)
		return NULL;
	if (paths->len == 0) {
		g_set_error_literal(error,
				    G_IO_ERROR,
				    G_IO_ERROR_NOT_FOUND,
				    "no keyboard detected");
		return NULL;
	}
	if (paths->len > 1) {
		g_set_error(error,
			    G_IO_ERROR,
			    G_IO_ERROR_EXISTS,
			    "too many devices: %u; set XYKBMS_HIDRAW to choose one",
			    paths->len);
		return NULL;
	}
	return xy_device_open(g_ptr_array_index(paths, 0), error);
}

void
xy_device_free(XyDevice *self)
{
	if (self->fd >= 0)
		close(self->fd);
	g_free(self->path);
	g_free(self);
}

const gchar *
xy_device_get_path(XyDevice *self)
{
	return self->path;
}

static void
xy_device_flush(XyDevice *self)
{
	guint8 buf[64];
	while (read(self->fd, buf, sizeof(buf)) > 0) {
	}
}

static void
xy_device_dump(const gchar *prefix, const guint8 *pkt)
{
	g_autoptr(GString) str = NULL;

	if (g_getenv("XYKBMS_VERBOSE") == NULL)
		return;
	str = g_string_new(prefix);
	for (guint i = 0; i < XY_PACKET_SIZE; i++)
		g_string_append_printf(str, " %02x", pkt[i]);
	g_printerr("%s\n", str->str);
}

static gboolean
xy_device_send(XyDevice *self, const guint8 *pkt, GError **error)
{
	gssize rc;

	xy_device_dump("→", pkt);

	/* drop any reply that arrived after a previous timeout */
	xy_device_flush(self);
	rc = write(self->fd, pkt, XY_PACKET_SIZE);
	if (rc != XY_PACKET_SIZE) {
		gint errsv = errno;
		g_set_error(error,
			    G_IO_ERROR,
			    rc < 0 ? g_io_error_from_errno(errsv) : G_IO_ERROR_PARTIAL_INPUT,
			    "failed to write: %s",
			    rc < 0 ? g_strerror(errsv) : "short write");
		return FALSE;
	}
	return TRUE;
}

static gboolean
xy_device_recv(XyDevice *self, guint8 *pkt, guint timeout_ms, GError **error)
{
	gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;

	memset(pkt, XY_FILL_BYTE, XY_PACKET_SIZE);
	for (;;) {
		guint8 buf[64];
		gssize rc;
		gint64 remaining = (deadline - g_get_monotonic_time()) / 1000;
		struct pollfd pfd = {.fd = self->fd, .events = POLLIN};

		if (remaining <= 0) {
			g_set_error_literal(error,
					    G_IO_ERROR,
					    G_IO_ERROR_TIMED_OUT,
					    "timed out waiting for keyboard");
			return FALSE;
		}
		rc = poll(&pfd, 1, (gint)remaining);
		if (rc < 0 && errno == EINTR)
			continue;
		if (rc < 0) {
			gint errsv = errno;
			g_set_error(error,
				    G_IO_ERROR,
				    g_io_error_from_errno(errsv),
				    "failed to poll: %s",
				    g_strerror(errsv));
			return FALSE;
		}
		if (rc == 0)
			continue;
		if (pfd.revents & (POLLERR | POLLHUP)) {
			g_set_error_literal(error,
					    G_IO_ERROR,
					    G_IO_ERROR_CONNECTION_CLOSED,
					    "keyboard was disconnected");
			return FALSE;
		}
		rc = read(self->fd, buf, sizeof(buf));
		if (rc < 0 && (errno == EAGAIN || errno == EINTR))
			continue;
		if (rc < 0) {
			gint errsv = errno;
			g_set_error(error,
				    G_IO_ERROR,
				    g_io_error_from_errno(errsv),
				    "failed to read: %s",
				    g_strerror(errsv));
			return FALSE;
		}

		/* the consumer-control collection shares this interface */
		if (rc < 1 || buf[0] != XY_REPORT_ID)
			continue;
		memcpy(pkt, buf, MIN((gsize)rc, XY_PACKET_SIZE));
		xy_device_dump("←", pkt);
		return TRUE;
	}
}

/* send a packet and wait for a reply, resending on timeout */
static gboolean
xy_device_transfer(XyDevice *self, const guint8 *req, guint8 *res, guint retries, GError **error)
{
	for (guint i = 0;; i++) {
		g_autoptr(GError) error_local = NULL;
		if (!xy_device_send(self, req, error))
			return FALSE;
		if (xy_device_recv(self, res, XY_TIMEOUT_MS, &error_local))
			return TRUE;
		if (i + 1 >= retries ||
		    !g_error_matches(error_local, G_IO_ERROR, G_IO_ERROR_TIMED_OUT)) {
			g_propagate_error(error, g_steal_pointer(&error_local));
			return FALSE;
		}
	}
}

static void
xy_device_set_error_not_found(GError **error)
{
	g_set_error_literal(error,
			    G_IO_ERROR,
			    G_IO_ERROR_NOT_FOUND,
			    "keyboard not found; if it is in low-power mode (TrackPoint off) "
			    "hold Fn and try again");
}

static void
xy_device_set_error_bad_state(GError **error)
{
	g_set_error_literal(error,
			    G_IO_ERROR,
			    G_IO_ERROR_FAILED,
			    "keyboard state is wrong; unplug and replug the receiver, then retry");
}

/* "02 3f <cmd>" -> "02 3f <cmd>" */
static gboolean
xy_device_ctrl(XyDevice *self, guint8 cmd, GError **error)
{
	guint8 req[XY_PACKET_SIZE] = {XY_REPORT_ID, XY_CMD_KIND_CTRL, cmd};
	guint8 res[XY_PACKET_SIZE];

	for (guint i = 0; i < XY_RETRIES_CMD; i++) {
		g_autoptr(GError) error_local = NULL;
		if (!xy_device_transfer(self, req, res, 1, &error_local)) {
			if (!g_error_matches(error_local, G_IO_ERROR, G_IO_ERROR_TIMED_OUT)) {
				g_propagate_error(error, g_steal_pointer(&error_local));
				return FALSE;
			}
			if (i + 1 < XY_RETRIES_CMD)
				continue;
			xy_device_set_error_not_found(error);
			return FALSE;
		}
		if (res[1] == XY_CMD_KIND_CTRL && res[2] == cmd)
			return TRUE;
		if (res[1] != XY_FILL_BYTE && res[2] != XY_FILL_BYTE)
			break;
	}
	xy_device_set_error_bad_state(error);
	return FALSE;
}

/*
 * "02 3e <cmd> <data>"; if @out is NULL the reply must echo @cmd, otherwise
 * the reply payload is copied to @out
 */
static gboolean
xy_device_data(XyDevice *self,
	       guint8 cmd,
	       const guint8 *data,
	       gsize datasz,
	       guint8 *out,
	       gsize outsz,
	       GError **error)
{
	guint8 req[XY_PACKET_SIZE] = {XY_REPORT_ID, XY_CMD_KIND_DATA, cmd};
	guint8 res[XY_PACKET_SIZE];

	g_return_val_if_fail(datasz <= XY_BLOCK_SIZE, FALSE);
	if (data != NULL)
		memcpy(req + 3, data, datasz);

	for (guint i = 0; i < XY_RETRIES_CMD; i++) {
		g_autoptr(GError) error_local = NULL;
		if (!xy_device_transfer(self, req, res, 1, &error_local)) {
			if (!g_error_matches(error_local, G_IO_ERROR, G_IO_ERROR_TIMED_OUT)) {
				g_propagate_error(error, g_steal_pointer(&error_local));
				return FALSE;
			}
			if (i + 1 < XY_RETRIES_CMD)
				continue;
			xy_device_set_error_not_found(error);
			return FALSE;
		}
		if (out != NULL) {
			memcpy(out, res + 1, MIN(outsz, XY_PACKET_SIZE - 1));
			return TRUE;
		}
		if (res[1] == cmd)
			return TRUE;
		if (res[1] != XY_FILL_BYTE || res[2] != XY_FILL_BYTE)
			break;
	}
	xy_device_set_error_bad_state(error);
	return FALSE;
}

gboolean
xy_device_read_config(XyDevice *self,
		      guint8 *cfg,
		      XyProgressFunc progress_cb,
		      gpointer user_data,
		      GError **error)
{
	guint16 idx = 0;
	guint8 tmp[XY_CFG_SIZE];

	if (!xy_device_ctrl(self, XY_CTRL_READ_CONFIG, error))
		return FALSE;
	memset(tmp, 0xff, sizeof(tmp));
	while (idx < XY_CFG_SIZE / XY_READ_BLOCK_SIZE) {
		guint8 req[XY_PACKET_SIZE] = {XY_REPORT_ID, idx & 0xff, idx >> 8};
		guint8 res[XY_PACKET_SIZE];
		guint16 ridx;

		if (progress_cb != NULL)
			progress_cb(idx * XY_READ_BLOCK_SIZE * 100 / XY_CFG_SIZE, user_data);
		if (!xy_device_transfer(self, req, res, XY_RETRIES_BULK, error))
			return FALSE;
		ridx = (guint16)res[1] | ((guint16)res[2] << 8);
		if ((guint)ridx * XY_READ_BLOCK_SIZE > XY_CFG_SIZE - XY_READ_BLOCK_SIZE) {
			g_set_error(error,
				    G_IO_ERROR,
				    G_IO_ERROR_INVALID_DATA,
				    "invalid packet ID 0x%04x",
				    ridx);
			return FALSE;
		}
		memcpy(tmp + ridx * XY_READ_BLOCK_SIZE, res + 3, XY_READ_BLOCK_SIZE);
		idx = ridx + 1;
	}

	/* end of transfer */
	{
		guint8 req[XY_PACKET_SIZE] = {XY_REPORT_ID, 0xff, 0xff};
		if (!xy_device_send(self, req, error))
			return FALSE;
	}
	if (progress_cb != NULL)
		progress_cb(100, user_data);
	if (!xy_config_validate(tmp, sizeof(tmp), error))
		return FALSE;
	memcpy(cfg, tmp, sizeof(tmp));
	return TRUE;
}

/*
 * Shared by config and firmware writes: an ack of "11 22" means continue,
 * anything else is the block the device wants next.
 */
static gboolean
xy_device_write_blocks(XyDevice *self,
		       const guint8 *buf,
		       gsize bufsz,
		       guint8 idx_hi_flags,
		       guint8 end_hi_flags,
		       XyProgressFunc progress_cb,
		       gpointer user_data,
		       GError **error)
{
	guint16 idx = 0;
	guint16 nblocks = bufsz / XY_BLOCK_SIZE;

	for (;;) {
		guint8 req[XY_PACKET_SIZE] = {XY_REPORT_ID};
		guint8 res[XY_PACKET_SIZE];

		if (progress_cb != NULL)
			progress_cb(MIN(idx, nblocks) * 100 / nblocks, user_data);
		req[1] = idx & 0xff;
		if (idx >= nblocks) {
			req[2] = (idx >> 8) | end_hi_flags;
			memset(req + 3, 0xff, XY_BLOCK_SIZE);
			return xy_device_send(self, req, error);
		}
		req[2] = (idx >> 8) | idx_hi_flags;
		memcpy(req + 3, buf + idx * XY_BLOCK_SIZE, XY_BLOCK_SIZE);
		if (!xy_device_transfer(self, req, res, XY_RETRIES_BULK, error))
			return FALSE;
		if (res[1] == 0x11 && res[2] == 0x22)
			idx++;
		else
			idx = (guint16)res[1] | ((guint16)res[2] << 8);
		idx &= 0x3fff;
	}
}

gboolean
xy_device_write_config(XyDevice *self,
		       const guint8 *cfg,
		       XyProgressFunc progress_cb,
		       gpointer user_data,
		       GError **error)
{
	/* the device does not check this itself */
	if (!xy_config_validate(cfg, XY_CFG_SIZE, error))
		return FALSE;
	if (!xy_device_ctrl(self, XY_CTRL_WRITE_CONFIG, error))
		return FALSE;
	/* unlike firmware, the end marker has no 0x80 flag (0x405cc2) */
	return xy_device_write_blocks(self,
				      cfg,
				      XY_CFG_SIZE,
				      0x00,
				      0x00,
				      progress_cb,
				      user_data,
				      error);
}

gboolean
xy_device_write_layers(XyDevice *self,
		       const guint8 *layers,
		       XyProgressFunc progress_cb,
		       gpointer user_data,
		       GError **error)
{
	guint16 idx = 0;

	if (!xy_device_ctrl(self, XY_CTRL_WRITE_LAYERS, error))
		return FALSE;
	while ((guint)idx * XY_BLOCK_SIZE < XY_LAYERS_SIZE) {
		guint8 req[XY_PACKET_SIZE] = {XY_REPORT_ID, idx & 0xff, idx >> 8};
		guint8 res[XY_PACKET_SIZE];

		if (progress_cb != NULL)
			progress_cb(idx * XY_BLOCK_SIZE * 100 / XY_LAYERS_SIZE, user_data);
		memcpy(req + 3, layers + idx * XY_BLOCK_SIZE, XY_BLOCK_SIZE);
		if (!xy_device_transfer(self, req, res, XY_RETRIES_BULK, error))
			return FALSE;
		idx = (guint16)res[1] | ((guint16)res[2] << 8);
	}

	/* end of transfer */
	{
		guint8 req[XY_PACKET_SIZE] = {XY_REPORT_ID, 0xff, 0xff};
		memset(req + 3, 0xff, XY_BLOCK_SIZE);
		if (!xy_device_send(self, req, error))
			return FALSE;
	}
	if (progress_cb != NULL)
		progress_cb(100, user_data);
	return TRUE;
}

gboolean
xy_device_write_firmware(XyDevice *self,
			 GBytes *fw,
			 guint16 fw_type,
			 XyProgressFunc progress_cb,
			 gpointer user_data,
			 GError **error)
{
	gsize bufsz = 0;
	const guint8 *buf = g_bytes_get_data(fw, &bufsz);

	if (bufsz == 0 || bufsz >= XY_FW_MAX_SIZE) {
		g_set_error(error,
			    G_IO_ERROR,
			    G_IO_ERROR_INVALID_DATA,
			    "firmware size 0x%x invalid, must be below 0x%x",
			    (guint)bufsz,
			    (guint)XY_FW_MAX_SIZE);
		return FALSE;
	}
	if (bufsz % XY_FW_ALIGN != 0) {
		g_set_error(error,
			    G_IO_ERROR,
			    G_IO_ERROR_INVALID_DATA,
			    "firmware size 0x%x invalid, must be a multiple of 0x%x",
			    (guint)bufsz,
			    (guint)XY_FW_ALIGN);
		return FALSE;
	}
	return xy_device_write_blocks(self,
				      buf,
				      bufsz,
				      fw_type >> 8,
				      0x80,
				      progress_cb,
				      user_data,
				      error);
}

gboolean
xy_device_restore_config(XyDevice *self, GError **error)
{
	guint8 req[XY_PACKET_SIZE] = {XY_REPORT_ID, 0x00, 0x80};
	memset(req + 3, 0xff, XY_BLOCK_SIZE);
	return xy_device_send(self, req, error);
}

gboolean
xy_device_enter_boot(XyDevice *self, gboolean bluetooth, GError **error)
{
	const guint8 magic[] = {0x11, 0x22, 0x33, 0x44, 0x55};
	if (bluetooth)
		return xy_device_data(self,
				      XY_DATA_ENTER_BOOT_BT,
				      magic,
				      sizeof(magic),
				      NULL,
				      0,
				      error);
	return xy_device_data(self, XY_DATA_ENTER_BOOT, NULL, 0, NULL, 0, error);
}

gboolean
xy_device_set_backlight(XyDevice *self, guint8 percentage, GError **error)
{
	return xy_device_data(self, XY_DATA_BACKLIGHT, &percentage, 1, NULL, 0, error);
}

gchar *
xy_device_get_version(XyDevice *self, GError **error)
{
	gchar buf[XY_PACKET_SIZE] = {0};
	if (!xy_device_data(self, XY_DATA_VERSION, NULL, 0, (guint8 *)buf, XY_PACKET_SIZE - 1, error))
		return NULL;
	return g_strndup(buf, XY_PACKET_SIZE - 1);
}

gboolean
xy_device_get_battery(XyDevice *self, guint32 *millivolts, GError **error)
{
	guint8 buf[4] = {0};
	if (!xy_device_data(self, XY_DATA_BATTERY, NULL, 0, buf, sizeof(buf), error))
		return FALSE;
	*millivolts = (guint32)buf[0] | ((guint32)buf[1] << 8) | ((guint32)buf[2] << 16) |
		      ((guint32)buf[3] << 24);
	return TRUE;
}

gboolean
xy_device_scancode_start(XyDevice *self, GError **error)
{
	return xy_device_data(self, XY_DATA_SCAN_START, NULL, 0, NULL, 0, error);
}

gboolean
xy_device_scancode_stop(XyDevice *self, GError **error)
{
	return xy_device_data(self, XY_DATA_SCAN_STOP, NULL, 0, NULL, 0, error);
}

/* returns the same text as the vendor tool, or NULL with G_IO_ERROR_TIMED_OUT */
gchar *
xy_device_scancode_read(XyDevice *self, guint timeout_ms, GError **error)
{
	guint8 res[XY_PACKET_SIZE];
	if (!xy_device_recv(self, res, timeout_ms, error))
		return NULL;
	return g_strdup_printf("ScanCode : %.2x %.2x %.2x %.2x %.2x %.3d %.2d %.2d",
			       res[1],
			       res[2],
			       res[3],
			       res[4],
			       res[5],
			       (gint8)res[6],
			       res[7],
			       res[8]);
}
