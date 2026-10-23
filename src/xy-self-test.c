/*
 * Copyright 2026 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "xy-config.h"
#include "xy-device.h"

/* a fake keyboard on the other end of a SOCK_SEQPACKET socketpair */
typedef struct {
	gint fd;
	guint8 cfg[XY_CFG_SIZE];
	guint8 cfg_new[XY_CFG_SIZE];
	guint8 fw[0x1000];
	guint8 last_ctrl;
	gboolean in_boot;
	gboolean cfg_written;
	gboolean fw_done;
	gsize fw_written;
	guint fw_type_hi;
	guint resend_once;
} XyFakeKeyboard;

static void
xy_fake_reply(XyFakeKeyboard *kb, const guint8 *res)
{
	guint8 buf[19] = {0x02};
	guint8 noise[3] = {0x01, 0xe9, 0x00};

	/* the consumer-control collection shares the interface and must be ignored */
	g_assert_cmpint(write(kb->fd, noise, sizeof(noise)), ==, sizeof(noise));
	memcpy(buf + 1, res, 18);
	g_assert_cmpint(write(kb->fd, buf, sizeof(buf)), ==, sizeof(buf));
}

static gpointer
xy_fake_keyboard_thread(gpointer user_data)
{
	XyFakeKeyboard *kb = user_data;
	guint8 req[19];

	while (read(kb->fd, req, sizeof(req)) == sizeof(req)) {
		guint8 res[18] = {0};
		guint16 idx = (guint16)req[1] | ((guint16)(req[2] & 0x3f) << 8);

		g_assert_cmpint(req[0], ==, 0x02);

		/* block indexes can look like commands, so check the state first */
		if (kb->last_ctrl == 0 && !kb->in_boot && req[1] == 0x3f) {
			kb->last_ctrl = req[2];
			res[0] = 0x3f;
			res[1] = req[2];
			xy_fake_reply(kb, res);
			continue;
		}
		if (kb->last_ctrl == 0 && !kb->in_boot && req[1] == 0x3e) {
			res[0] = req[2];
			if (req[2] == 0x6b)
				memcpy(res, "2020.05.12", 10);
			if (req[2] == 0x6a) {
				res[0] = 0x1c; /* 3868 mV */
				res[1] = 0x0f;
			}
			if (req[2] == 0xcc)
				kb->in_boot = TRUE;
			xy_fake_reply(kb, res);
			continue;
		}

		/* read config */
		if (kb->last_ctrl == 0x22) {
			if (req[1] == 0xff && req[2] == 0xff) {
				kb->last_ctrl = 0;
				continue;
			}
			res[0] = req[1];
			res[1] = req[2];
			memcpy(res + 2, kb->cfg + idx * 8, 8);
			xy_fake_reply(kb, res);
			continue;
		}

		/* write config: the end marker is block 64 with no flags */
		if (kb->last_ctrl == 0x11) {
			g_assert_cmpint(req[2] & 0x80, ==, 0);
			if (idx == XY_CFG_SIZE / 16) {
				kb->cfg_written = TRUE;
				kb->last_ctrl = 0;
				continue;
			}
			memcpy(kb->cfg_new + idx * 16, req + 3, 16);
		} else {
			/* firmware: the end marker has 0x80 set */
			if (req[2] & 0x80) {
				kb->fw_done = TRUE;
				continue;
			}
			kb->fw_type_hi = req[2] & 0x40;
			memcpy(kb->fw + idx * 16, req + 3, 16);
			kb->fw_written = MAX(kb->fw_written, (gsize)(idx + 1) * 16);
		}

		/* ask for block 3 again, once */
		if (idx == 4 && kb->resend_once > 0) {
			kb->resend_once--;
			res[0] = 3;
			res[1] = 0;
		} else {
			res[0] = 0x11;
			res[1] = 0x22;
		}
		xy_fake_reply(kb, res);
	}
	return NULL;
}

static XyDevice *
xy_fake_keyboard_new(XyFakeKeyboard *kb, GThread **thread)
{
	gint fds[2];
	g_assert_cmpint(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fds), ==, 0);
	kb->fd = fds[1];
	*thread = g_thread_new("fake-keyboard", xy_fake_keyboard_thread, kb);
	return xy_device_new_for_fd(fds[0]);
}

static void
xy_fake_keyboard_stop(XyFakeKeyboard *kb, XyDevice *device, GThread *thread)
{
	xy_device_free(device);
	g_thread_join(thread);
	close(kb->fd);
}

static void
xy_test_crc(void)
{
	g_assert_cmphex(xy_crc32((const guint8 *)"123456789", 9), ==, 0xcbf43926);
}

static void
xy_test_cfg_parse(void)
{
	const gchar *data = "; comment 0x99\n0x01 0x02 ;0x03\n  0x1234\t0xabcd\n";
	g_autoptr(GError) error = NULL;
	g_autoptr(GByteArray) buf = xy_cfg_parse(data, strlen(data), &error);
	const guint8 expected[] = {0x01, 0x02, 0x34, 0x12, 0xcd, 0xab};

	g_assert_no_error(error);
	g_assert_nonnull(buf);
	g_assert_cmpmem(buf->data, buf->len, expected, sizeof(expected));

	g_clear_pointer(&buf, g_byte_array_unref);
	buf = xy_cfg_parse("0x123", 5, &error);
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
	g_assert_null(buf);
}

static void
xy_test_vendor_files(void)
{
	const gchar *name;
	const gchar *datadir = g_getenv("XY_TEST_DATA");
	guint cnt = 0;
	g_autoptr(GDir) dir = NULL;
	g_autoptr(GError) error_dir = NULL;

	if (datadir == NULL) {
		g_test_skip("XY_TEST_DATA not set");
		return;
	}
	dir = g_dir_open(datadir, 0, &error_dir);
	g_assert_no_error(error_dir);
	while ((name = g_dir_read_name(dir)) != NULL) {
		guint8 cfg[XY_CFG_SIZE] = {XY_CFG_VERSION};
		g_autoptr(GError) error = NULL;
		g_autofree gchar *fn = g_build_filename(datadir, name, NULL);
		if (!g_str_has_suffix(name, ".cfg"))
			continue;
		g_test_message("parsing %s", name);
		g_assert_true(xy_config_apply_pin_file(cfg, fn, &error));
		g_assert_no_error(error);
		g_assert_true(xy_config_validate(cfg, sizeof(cfg), &error));
		cnt++;
	}
	g_assert_cmpuint(cnt, >, 0);

	/* a config saved by the vendor tool, if available */
	{
		gsize bufsz = 0;
		g_autofree gchar *buf = NULL;
		g_autoptr(GError) error = NULL;
		g_autofree gchar *fn = g_build_filename(datadir, "xykbms_config.bin", NULL);
		if (!g_file_test(fn, G_FILE_TEST_EXISTS))
			return;
		g_assert_true(g_file_get_contents(fn, &buf, &bufsz, &error));
		g_assert_true(xy_config_validate((const guint8 *)buf, bufsz, &error));
		g_assert_no_error(error);
		g_assert_cmpstr(xy_board_type_to_string(buf[XY_CFG_OFF_BOARD]), ==, "Wired USB board");
	}
}

static void
xy_test_protocol(void)
{
	XyFakeKeyboard kb = {.resend_once = 1};
	GThread *thread = NULL;
	guint8 cfg[XY_CFG_SIZE];
	guint32 mv = 0;
	g_autofree gchar *version = NULL;
	g_autoptr(GBytes) fw = NULL;
	g_autoptr(GError) error = NULL;
	XyDevice *device = xy_fake_keyboard_new(&kb, &thread);

	/* config the keyboard already has */
	for (guint i = 0; i < sizeof(kb.cfg); i++)
		kb.cfg[i] = i & 0xff;
	kb.cfg[XY_CFG_OFF_VERSION] = XY_CFG_VERSION;
	xy_config_update_crc(kb.cfg);

	version = xy_device_get_version(device, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(version, ==, "2020.05.12");
	g_assert_true(xy_device_get_battery(device, &mv, &error));
	g_assert_cmpuint(mv, ==, 3868);

	g_assert_true(xy_device_read_config(device, cfg, NULL, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpmem(cfg, sizeof(cfg), kb.cfg, sizeof(kb.cfg));

	cfg[XY_CFG_OFF_MOVE_SPEED] = 0x80;
	xy_config_update_crc(cfg);
	g_assert_true(xy_device_write_config(device, cfg, NULL, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpmem(kb.cfg_new, sizeof(kb.cfg_new), cfg, sizeof(cfg));

	g_assert_true(xy_device_enter_boot(device, FALSE, &error));
	g_assert_no_error(error);

	/* firmware must be a multiple of 2 KiB */
	{
		guint8 buf[0x1000];
		for (guint i = 0; i < sizeof(buf); i++)
			buf[i] = (i * 7) & 0xff;
		fw = g_bytes_new(buf, sizeof(buf));
		kb.resend_once = 1;
		g_assert_true(
		    xy_device_write_firmware(device, fw, XY_FIRMWARE_TYPE_USB, NULL, NULL, &error));
		g_assert_no_error(error);
		g_assert_cmpmem(kb.fw, sizeof(kb.fw), buf, sizeof(buf));
		g_assert_cmpuint(kb.fw_type_hi, ==, 0x40);
	}

	xy_fake_keyboard_stop(&kb, device, thread);
	g_assert_true(kb.cfg_written);
	g_assert_true(kb.fw_done);
}

static void
xy_test_firmware_size(void)
{
	g_autoptr(GBytes) fw = g_bytes_new_static("abc", 3);
	g_autoptr(GError) error = NULL;
	g_autoptr(XyDevice) device = xy_device_new_for_fd(-1);

	g_assert_false(
	    xy_device_write_firmware(device, fw, XY_FIRMWARE_TYPE_APP, NULL, NULL, &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/xy/crc", xy_test_crc);
	g_test_add_func("/xy/cfg-parse", xy_test_cfg_parse);
	g_test_add_func("/xy/vendor-files", xy_test_vendor_files);
	g_test_add_func("/xy/protocol", xy_test_protocol);
	g_test_add_func("/xy/firmware-size", xy_test_firmware_size);
	return g_test_run();
}
