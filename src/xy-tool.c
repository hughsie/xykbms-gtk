/*
 * Copyright 2026 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <stdlib.h>
#include <string.h>

#include "xy-config.h"
#include "xy-device.h"

static void
xy_tool_progress_cb(guint percentage, gpointer user_data)
{
	g_print("\r%3u%%", percentage);
	if (percentage == 100)
		g_print("\n");
}

static void
xy_tool_print_config(const guint8 *cfg)
{
	const gchar *board = xy_board_type_to_string(cfg[XY_CFG_OFF_BOARD]);
	g_autofree gchar *name =
	    g_strndup((const gchar *)cfg + XY_CFG_OFF_BT_NAME, XY_CFG_BT_NAME_MAX + 1);

	g_print("Board:            %s (%u)\n",
		board != NULL ? board : "Unknown",
		cfg[XY_CFG_OFF_BOARD]);
	g_print("Bluetooth name:   %s\n", g_utf8_validate(name, -1, NULL) ? name : "?");
	g_print("Auto-off:         %u min\n", xy_config_get_u16(cfg, XY_CFG_OFF_AUTO_OFF));
	g_print("TP auto-off:      %u min\n", xy_config_get_u16(cfg, XY_CFG_OFF_TP_AUTO_OFF));
	g_print("Move speed:       %u\n", cfg[XY_CFG_OFF_MOVE_SPEED]);
	g_print("Scroll speed:     %u\n", cfg[XY_CFG_OFF_SCROLL_SPEED]);
	g_print("Wireless mode:    %u\n", cfg[XY_CFG_OFF_RF_MODE]);
	g_print("Wireless channel: %u\n", cfg[XY_CFG_OFF_RF_CHANNEL] + 1u);
	g_print("Flags:            0x%02x 0x%02x\n",
		cfg[XY_CFG_OFF_FLAGS_A],
		cfg[XY_CFG_OFF_FLAGS_B]);
}

static gboolean
xy_tool_load_config(const gchar *filename, guint8 *cfg, GError **error)
{
	gsize bufsz = 0;
	g_autofree gchar *buf = NULL;

	if (!g_file_get_contents(filename, &buf, &bufsz, error))
		return FALSE;
	if (!xy_config_validate((const guint8 *)buf, bufsz, error))
		return FALSE;
	memcpy(cfg, buf, XY_CFG_SIZE);
	return TRUE;
}

static void
xy_tool_usage(void)
{
	g_printerr(
	    "Usage: xykbms-tool COMMAND [ARGS]\n"
	    "\n"
	    "  list                          List matching hidraw devices\n"
	    "  version                       Read the firmware version\n"
	    "  battery                       Read the battery voltage\n"
	    "  read-config FILE              Read the keyboard config into FILE\n"
	    "  show-config FILE              Show a saved config file\n"
	    "  write-config FILE [PIN.cfg]   Write a saved config, optionally with a pin config\n"
	    "  write-layers LAYERS.cfg       Write a layer configuration\n"
	    "  parse-cfg PIN.cfg             Check a pin configuration file\n"
	    "  scancodes                     Print scan codes until Ctrl+C\n"
	    "  backlight PERCENT             Set the backlight level\n"
	    "  enter-boot [--bluetooth]      Reboot into BOOT mode\n"
	    "  flash-boot FILE               (BOOT mode) flash xykbms_bootupdate_release.bin\n"
	    "  flash-app FILE                (BOOT mode) flash xykbms_app_release.bin\n"
	    "  flash-usb FILE                (BOOT mode) flash xykbms_usb_release.bin\n"
	    "  restore                       (BOOT mode) clear pairing and user settings\n"
	    "\n"
	    "Set XYKBMS_HIDRAW=/dev/hidrawN to choose a device, XYKBMS_VERBOSE=1 to dump "
	    "packets.\n");
}

static gboolean
xy_tool_run(gint argc, gchar **argv, GError **error)
{
	const gchar *cmd = argv[1];
	g_autoptr(XyDevice) device = NULL;

	/* commands that do not need the device */
	if (g_strcmp0(cmd, "list") == 0) {
		g_autoptr(GPtrArray) paths = xy_device_enumerate(error);
		if (paths == NULL)
			return FALSE;
		for (guint i = 0; i < paths->len; i++)
			g_print("%s\n", (const gchar *)g_ptr_array_index(paths, i));
		return TRUE;
	}
	if (g_strcmp0(cmd, "parse-cfg") == 0 && argc == 3) {
		guint8 cfg[XY_CFG_SIZE] = {XY_CFG_VERSION};
		if (!xy_config_apply_pin_file(cfg, argv[2], error))
			return FALSE;
		g_print("OK: %u bytes of pin configuration\n", (guint)XY_CFG_PIN_SIZE);
		return TRUE;
	}
	if (g_strcmp0(cmd, "show-config") == 0 && argc == 3) {
		guint8 cfg[XY_CFG_SIZE];
		if (!xy_tool_load_config(argv[2], cfg, error))
			return FALSE;
		xy_tool_print_config(cfg);
		return TRUE;
	}

	device = xy_device_open_default(error);
	if (device == NULL)
		return FALSE;

	if (g_strcmp0(cmd, "version") == 0) {
		g_autofree gchar *version = xy_device_get_version(device, error);
		if (version == NULL)
			return FALSE;
		g_print("%s\n", version);
		return TRUE;
	}
	if (g_strcmp0(cmd, "battery") == 0) {
		guint32 mv = 0;
		if (!xy_device_get_battery(device, &mv, error))
			return FALSE;
		g_print("%u mV\n", mv);
		return TRUE;
	}
	if (g_strcmp0(cmd, "read-config") == 0 && argc == 3) {
		guint8 cfg[XY_CFG_SIZE];
		if (!xy_device_read_config(device, cfg, xy_tool_progress_cb, NULL, error))
			return FALSE;
		xy_tool_print_config(cfg);
		return g_file_set_contents(argv[2], (const gchar *)cfg, sizeof(cfg), error);
	}
	if (g_strcmp0(cmd, "write-config") == 0 && (argc == 3 || argc == 4)) {
		guint8 cfg[XY_CFG_SIZE];
		if (!xy_tool_load_config(argv[2], cfg, error))
			return FALSE;
		if (argc == 4 && !xy_config_apply_pin_file(cfg, argv[3], error))
			return FALSE;
		return xy_device_write_config(device, cfg, xy_tool_progress_cb, NULL, error);
	}
	if (g_strcmp0(cmd, "write-layers") == 0 && argc == 3) {
		guint8 layers[XY_LAYERS_SIZE];
		if (!xy_layers_load(layers, argv[2], error))
			return FALSE;
		return xy_device_write_layers(device, layers, xy_tool_progress_cb, NULL, error);
	}
	if (g_strcmp0(cmd, "scancodes") == 0) {
		g_autofree gchar *last = NULL;
		if (!xy_device_scancode_start(device, error))
			return FALSE;
		for (;;) {
			g_autoptr(GError) error_local = NULL;
			g_autofree gchar *str = xy_device_scancode_read(device, 1000, &error_local);
			if (str == NULL) {
				if (g_error_matches(error_local, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
					continue;
				g_propagate_error(error, g_steal_pointer(&error_local));
				return FALSE;
			}
			/* the firmware streams reports continuously; only show changes */
			if (!g_set_str(&last, str))
				continue;
			g_print("%s\n", str);
		}
	}
	if (g_strcmp0(cmd, "backlight") == 0 && argc == 3) {
		guint64 val = 0;
		if (!g_ascii_string_to_unsigned(argv[2], 10, 0, 100, &val, error))
			return FALSE;
		return xy_device_set_backlight(device, val, error);
	}
	if (g_strcmp0(cmd, "enter-boot") == 0) {
		return xy_device_enter_boot(device,
					    argc == 3 && g_strcmp0(argv[2], "--bluetooth") == 0,
					    error);
	}
	if ((g_strcmp0(cmd, "flash-boot") == 0 || g_strcmp0(cmd, "flash-app") == 0 ||
	     g_strcmp0(cmd, "flash-usb") == 0) &&
	    argc == 3) {
		gsize bufsz = 0;
		g_autofree gchar *buf = NULL;
		g_autoptr(GBytes) fw = NULL;
		guint16 fw_type = g_strcmp0(cmd, "flash-usb") == 0 ? XY_FIRMWARE_TYPE_USB
								    : XY_FIRMWARE_TYPE_APP;
		if (!g_file_get_contents(argv[2], &buf, &bufsz, error))
			return FALSE;
		fw = g_bytes_new_take(g_steal_pointer(&buf), bufsz);
		return xy_device_write_firmware(device, fw, fw_type, xy_tool_progress_cb, NULL, error);
	}
	if (g_strcmp0(cmd, "restore") == 0)
		return xy_device_restore_config(device, error);

	xy_tool_usage();
	exit(EXIT_FAILURE);
}

int
main(int argc, char **argv)
{
	g_autoptr(GError) error = NULL;

	if (argc < 2) {
		xy_tool_usage();
		return EXIT_FAILURE;
	}
	if (!xy_tool_run(argc, argv, &error)) {
		g_printerr("Error: %s\n", error->message);
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
