/*
 * Copyright 2026 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <gio/gio.h>
#include <string.h>
#include <zlib.h>

#include "xy-config.h"

static const guint8 xy_sentinel[] = {0xaa, 0xbb, 0xcc, 0xdd};

/* standard CRC-32, which the device stores over the first 128 bytes */
guint32
xy_crc32(const guint8 *buf, gsize bufsz)
{
	return crc32(0L, buf, bufsz);
}

guint16
xy_config_get_u16(const guint8 *cfg, gsize offset)
{
	return (guint16)cfg[offset] | ((guint16)cfg[offset + 1] << 8);
}

void
xy_config_set_u16(guint8 *cfg, gsize offset, guint16 value)
{
	cfg[offset] = value & 0xff;
	cfg[offset + 1] = value >> 8;
}

/* the vendor tool only checksums the first 128 bytes */
void
xy_config_update_crc(guint8 *cfg)
{
	guint32 crc = xy_crc32(cfg, XY_CFG_CRC_LEN);
	cfg[XY_CFG_OFF_CRC + 0] = crc & 0xff;
	cfg[XY_CFG_OFF_CRC + 1] = (crc >> 8) & 0xff;
	cfg[XY_CFG_OFF_CRC + 2] = (crc >> 16) & 0xff;
	cfg[XY_CFG_OFF_CRC + 3] = crc >> 24;
}

gboolean
xy_config_validate(const guint8 *cfg, gsize cfgsz, GError **error)
{
	guint32 crc;
	guint32 crc_stored;

	if (cfgsz != XY_CFG_SIZE) {
		g_set_error(error,
			    G_IO_ERROR,
			    G_IO_ERROR_INVALID_DATA,
			    "config has wrong length: 0x%x, expected 0x%x",
			    (guint)cfgsz,
			    (guint)XY_CFG_SIZE);
		return FALSE;
	}
	crc = xy_crc32(cfg, XY_CFG_CRC_LEN);
	crc_stored = (guint32)cfg[XY_CFG_OFF_CRC] | ((guint32)cfg[XY_CFG_OFF_CRC + 1] << 8) |
		     ((guint32)cfg[XY_CFG_OFF_CRC + 2] << 16) |
		     ((guint32)cfg[XY_CFG_OFF_CRC + 3] << 24);
	if (crc != crc_stored) {
		g_set_error(error,
			    G_IO_ERROR,
			    G_IO_ERROR_INVALID_DATA,
			    "config checksum invalid: 0x%08x, expected 0x%08x; please read again",
			    crc_stored,
			    crc);
		return FALSE;
	}
	if (cfg[XY_CFG_OFF_VERSION] != XY_CFG_VERSION) {
		g_set_error(error,
			    G_IO_ERROR,
			    G_IO_ERROR_NOT_SUPPORTED,
			    "config version %u not supported",
			    cfg[XY_CFG_OFF_VERSION]);
		return FALSE;
	}
	return TRUE;
}

const gchar *
xy_board_type_to_string(guint8 board)
{
	switch (board) {
	case XY_BOARD_DEBUG:
		return "Debug board";
	case XY_BOARD_TP_WIRELESS:
		return "ThinkPad wireless conversion board";
	case XY_BOARD_MECH_WIRELESS:
		return "Mechanical keyboard wireless conversion board";
	case XY_BOARD_TP_MECH:
		return "TrackPoint mechanical keyboard board";
	case XY_BOARD_TP60_MECH:
		return "TrackPoint 60% mechanical keyboard board";
	case XY_BOARD_TP_WIRELESS2:
		return "ThinkPad wireless conversion board 2";
	case XY_BOARD_TP60_MECH2:
		return "TrackPoint 60% mechanical keyboard board 2";
	case XY_BOARD_TP_WIRELESS3:
		return "ThinkPad wireless conversion board 3";
	case XY_BOARD_WIRED_USB:
		return "Wired USB board";
	default:
		return NULL;
	}
}

/*
 * Each "0xNN" token is one byte and each "0xNNNN" token is a little-endian
 * word; anything after ';' on a line is a comment.
 */
GByteArray *
xy_cfg_parse(const gchar *text, gsize textsz, GError **error)
{
	g_autoptr(GByteArray) buf = g_byte_array_new();
	guint lineno = 1;

	for (gsize i = 0; i < textsz; i++) {
		gsize ndigits = 0;
		guint value = 0;

		if (text[i] == '\n') {
			lineno++;
			continue;
		}
		if (text[i] == ';') {
			while (i + 1 < textsz && text[i + 1] != '\n')
				i++;
			continue;
		}
		if (text[i] != '0' || i + 1 >= textsz || (text[i + 1] != 'x' && text[i + 1] != 'X'))
			continue;
		i += 2;
		while (i < textsz && g_ascii_isxdigit(text[i]) && ndigits < 5) {
			value = (value << 4) | g_ascii_xdigit_value(text[i]);
			ndigits++;
			i++;
		}
		i--;
		if (ndigits == 2) {
			guint8 b = value;
			g_byte_array_append(buf, &b, 1);
		} else if (ndigits == 4) {
			guint8 w[] = {value & 0xff, value >> 8};
			g_byte_array_append(buf, w, sizeof(w));
		} else {
			g_set_error(error,
				    G_IO_ERROR,
				    G_IO_ERROR_INVALID_DATA,
				    "line %u: hex value must have 2 or 4 digits",
				    lineno);
			return NULL;
		}
	}
	return g_steal_pointer(&buf);
}

static GByteArray *
xy_cfg_parse_file(const gchar *filename, GError **error)
{
	gsize textsz = 0;
	g_autofree gchar *text = NULL;

	if (!g_file_get_contents(filename, &text, &textsz, error))
		return NULL;
	return xy_cfg_parse(text, textsz, error);
}

gboolean
xy_config_apply_pin_file(guint8 *cfg, const gchar *filename, GError **error)
{
	guint8 tmp[XY_CFG_SIZE];
	g_autoptr(GByteArray) buf = NULL;

	buf = xy_cfg_parse_file(filename, error);
	if (buf == NULL)
		return FALSE;
	if (buf->len != XY_CFG_PIN_SIZE) {
		g_set_error(error,
			    G_IO_ERROR,
			    G_IO_ERROR_INVALID_DATA,
			    "pin configuration has %u bytes, expected %u",
			    buf->len,
			    (guint)XY_CFG_PIN_SIZE);
		return FALSE;
	}
	memcpy(tmp, cfg, sizeof(tmp));
	memcpy(tmp + XY_CFG_PIN_OFFSET, buf->data, buf->len);
	if (memcmp(tmp + XY_CFG_OFF_SENTINEL, xy_sentinel, sizeof(xy_sentinel)) != 0) {
		g_set_error_literal(error,
				    G_IO_ERROR,
				    G_IO_ERROR_INVALID_DATA,
				    "pin configuration is missing the 0xaa 0xbb 0xcc 0xdd marker");
		return FALSE;
	}
	if (tmp[XY_CFG_OFF_PIN_VERSION] != XY_CFG_VERSION) {
		g_set_error(error,
			    G_IO_ERROR,
			    G_IO_ERROR_NOT_SUPPORTED,
			    "pin configuration version %u not supported",
			    tmp[XY_CFG_OFF_PIN_VERSION]);
		return FALSE;
	}
	memcpy(cfg, tmp, sizeof(tmp));
	xy_config_update_crc(cfg);
	return TRUE;
}

gboolean
xy_layers_load(guint8 *layers, const gchar *filename, GError **error)
{
	g_autoptr(GByteArray) buf = NULL;

	buf = xy_cfg_parse_file(filename, error);
	if (buf == NULL)
		return FALSE;
	if (buf->len != XY_LAYERS_SIZE) {
		g_set_error(error,
			    G_IO_ERROR,
			    G_IO_ERROR_INVALID_DATA,
			    "layer configuration has %u bytes, expected %u",
			    buf->len,
			    (guint)XY_LAYERS_SIZE);
		return FALSE;
	}
	if (memcmp(buf->data + XY_LAYERS_SIZE - sizeof(xy_sentinel),
		   xy_sentinel,
		   sizeof(xy_sentinel)) != 0) {
		g_set_error_literal(error,
				    G_IO_ERROR,
				    G_IO_ERROR_INVALID_DATA,
				    "layer configuration is missing the 0xaa 0xbb 0xcc 0xdd marker");
		return FALSE;
	}
	memcpy(layers, buf->data, XY_LAYERS_SIZE);
	return TRUE;
}
