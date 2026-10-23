/*
 * Copyright 2026 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

#define XY_CFG_SIZE	   0x400
#define XY_CFG_PIN_OFFSET  0x034
#define XY_CFG_PIN_SIZE	   0x3c8
#define XY_CFG_CRC_LEN	   0x080
#define XY_CFG_VERSION	   4
#define XY_LAYERS_SIZE	   0x800

#define XY_CFG_OFF_VERSION	0x000
#define XY_CFG_OFF_BOARD	0x001
#define XY_CFG_OFF_AUTO_OFF	0x002
#define XY_CFG_OFF_TP_AUTO_OFF	0x004
#define XY_CFG_OFF_MAGIC	0x006
#define XY_CFG_OFF_SCROLL_SPEED 0x008
#define XY_CFG_OFF_MOVE_SPEED	0x009
#define XY_CFG_OFF_FLAGS_A	0x00a
#define XY_CFG_OFF_FLAGS_B	0x00b
#define XY_CFG_OFF_BT_NAME	0x00c
#define XY_CFG_OFF_RF_MODE	0x01c
#define XY_CFG_OFF_RF_CHANNEL	0x01d
#define XY_CFG_OFF_SENTINEL	0x3e7
#define XY_CFG_OFF_PIN_VERSION	0x3fb
#define XY_CFG_OFF_CRC		0x3fc

#define XY_CFG_BT_NAME_MAX 14

/* flags A, at XY_CFG_OFF_FLAGS_A */
#define XY_CFG_FLAG_A_TP_MIDDLE_SCROLL	   (1u << 0)
#define XY_CFG_FLAG_A_TP_DOUBLE_MIDDLE_BKSP (1u << 1)
#define XY_CFG_FLAG_A_MIDDLE_BKSP	   (1u << 2)
#define XY_CFG_FLAG_A_LOW_POWER_KEEP_LED   (1u << 3)
#define XY_CFG_FLAG_A_DISABLE_WIN	   (1u << 4)
#define XY_CFG_FLAG_A_TP_ON_AT_POWER_UP	   (1u << 5)
#define XY_CFG_FLAG_A_TP_INVERT_XY	   (1u << 6)
#define XY_CFG_FLAG_A_USB_LOW_POWER	   (1u << 7)

/* flags B, at XY_CFG_OFF_FLAGS_B */
#define XY_CFG_FLAG_B_DISABLE_STA_BLINK (1u << 0)
#define XY_CFG_FLAG_B_STA_OFF		(1u << 1)
#define XY_CFG_FLAG_B_SWAP_FN_LCTRL	(1u << 2)
#define XY_CFG_FLAG_B_BT_PASSKEY	(1u << 3)
#define XY_CFG_FLAG_B_SPECIFIC_WAKE	(1u << 4)

typedef enum {
	XY_BOARD_DEBUG = 0,
	XY_BOARD_TP_WIRELESS = 1,
	XY_BOARD_MECH_WIRELESS = 2,
	XY_BOARD_TP_MECH = 3,
	XY_BOARD_TP60_MECH = 4,
	XY_BOARD_TP_WIRELESS2 = 5,
	XY_BOARD_TP60_MECH2 = 6,
	XY_BOARD_TP_WIRELESS3 = 7,
	XY_BOARD_WIRED_USB = 10,
} XyBoardType;

typedef enum {
	XY_RF_MODE_BT4 = 1,
	XY_RF_MODE_USB = 2,
	XY_RF_MODE_URT = 3,
} XyRfMode;

guint32
xy_crc32(const guint8 *buf, gsize bufsz);
void
xy_config_update_crc(guint8 *cfg);
gboolean
xy_config_validate(const guint8 *cfg, gsize cfgsz, GError **error);
const gchar *
xy_board_type_to_string(guint8 board);

guint16
xy_config_get_u16(const guint8 *cfg, gsize offset);
void
xy_config_set_u16(guint8 *cfg, gsize offset, guint16 value);

GByteArray *
xy_cfg_parse(const gchar *text, gsize textsz, GError **error);
gboolean
xy_config_apply_pin_file(guint8 *cfg, const gchar *filename, GError **error);
gboolean
xy_layers_load(guint8 *layers, const gchar *filename, GError **error);
