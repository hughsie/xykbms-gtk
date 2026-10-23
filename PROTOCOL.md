# XYKBMS HID protocol

Reverse engineered from `XYKBMS_SwUpdate.exe` (MFC, built 2020-05-12, source
`D:\work\MyKBMS\SVNBucket\software\SwUpdate\SwUpdateDlg.cpp`). Function
addresses refer to that binary.

## Transport

The Windows tool opens the HID collection whose path contains
`hid#vid_5859&pid_0001&mi_02&col02`, i.e. USB interface 2, the vendor-defined
collection:

```
06 00 ff   Usage Page (0xFF00)
85 02      Report ID (2)
95 12      Report Count (18), Report Size (8)  -> Input and Output
```

Every transfer is a 19-byte report: `[0x02, b1 … b18]`. Interface 2 also
carries a consumer-control collection (report ID 1); on Linux both arrive on the
same hidraw node, so reports whose first byte isn't `0x02` must be ignored.

Every read uses a 2000 ms timeout. Command packets are retried up to 3 times;
bulk-transfer packets up to 6 times.

## Command packets

There are two kinds of command packet:

| Kind       | Request                    | Success reply           | Function   |
|------------|----------------------------|-------------------------|------------|
| control    | `02 3f <cmd> 00…`          | `02 3f <cmd> …`         | `0x403440` |
| data       | `02 3e <cmd> <data ≤16>`   | `02 <cmd> …` or payload | `0x403680` |

A reply containing `0xfd` (the read buffer's fill byte) means "try again". Any
other mismatch is reported as "keyboard state wrong, replug the receiver".

### Control commands

| cmd    | Meaning                                                 |
|--------|---------------------------------------------------------|
| `0x22` | begin config read (followed by the config read stream)  |
| `0x11` | begin config write (followed by the config write stream)|
| `0x33` | begin layer write (followed by the layer write stream)  |

### Data commands

| cmd    | Payload           | Reply                                     |
|--------|-------------------|-------------------------------------------|
| `0xcc` | –                 | `02 cc` – reboot into BOOT                 |
| `0xcd` | `11 22 33 44 55`  | `02 cd` – reboot into BOOT (Bluetooth)     |
| `0x66` | `<percent>`       | `02 66` – set backlight level (0–100)      |
| `0x6b` | –                 | `02 <18 bytes ASCII>` – firmware version   |
| `0x6a` | –                 | `02 <u32 LE>` – battery voltage in mV      |
| `0x44` | –                 | `02 44` – start scancode reporting         |
| `0x45` | –                 | `02 45` – stop scancode reporting          |

While scancode reporting is on, the keyboard sends unsolicited reports. The
tool prints them as `%.2x %.2x %.2x %.2x %.2x %.3d %.2d %.2d` over
`b1…b8`, where `b6` is signed.

## Config read (`0x407060`)

1. control `0x22`
2. for `id = 0 … 127`: send `02 <id lo> <id hi>`; reply is
   `02 <rid lo> <rid hi> <8 bytes>`, stored at `cfg[rid * 8]`; then `id = rid + 1`
3. send `02 ff ff`
4. check `crc32(cfg[0x000:0x080]) == le32(cfg[0x3fc])` and `cfg[0] == 4`

## Config write (`0x405ba0`)

1. control `0x11`
2. `idx = 0`; while `idx < 64`: send `02 <idx lo> <idx hi> <cfg[idx*16 : +16]>`.
   If the reply is `02 11 22`, then `idx++`; otherwise
   `idx = (b1 | b2 << 8) & 0x3fff` (the device asks for a specific block).
3. send `02 <idx lo> <idx hi> ff×16` (idx = 64; no `0x80` flag, unlike firmware)

## Layer write (`0x407580`)

Same as config write but with control `0x33`, 2048 bytes of data, and
`idx = reply` after every block. The stream ends with `02 ff ff ff×16`.

## Firmware update (`0x404fc0`)

Only works while the keyboard is in BOOT mode. No control command is sent.

* file size must be a multiple of 2048 and below 128 KiB
* `type` is `0x0000` for `xykbms_bootupdate_release.bin` and
  `xykbms_app_release.bin`, and `0x4000` for `xykbms_usb_release.bin`
* data: `02 <idx lo> <(idx >> 8) | (type >> 8)> <16 bytes>`, with the same
  `11 22` / jump-to-block ack handling as the config write
* end: `02 <idx lo> <(idx >> 8) | 0x80> ff×16` (note: the config write end marker has **no** `0x80`)

"Restore config" (in BOOT) sends just `02 00 80 ff×16`.

## Config block (1024 bytes)

| Offset  | Size | Field                                                |
|---------|------|------------------------------------------------------|
| `0x000` | 1    | version, must be `4`                                 |
| `0x001` | 1    | board type (see below)                               |
| `0x002` | 2    | keyboard auto-off time, minutes (LE)                 |
| `0x004` | 2    | TrackPoint auto-off time, minutes (LE)               |
| `0x006` | 2    | always written as `0x3840`                           |
| `0x008` | 1    | scroll speed (1–15)                                  |
| `0x009` | 1    | TrackPoint move speed (80–255)                       |
| `0x00a` | 1    | flags A                                              |
| `0x00b` | 1    | flags B                                              |
| `0x00c` | 16   | Bluetooth name, NUL-terminated, 1–14 chars           |
| `0x01c` | 1    | default wireless mode: 1 = BT4, 2 = USB, 3 = URT     |
| `0x01d` | 1    | default channel (0–3)                                |
| `0x034` | 968  | pin configuration – the parsed `.cfg` file           |
| `0x3e7` | 4    | `aa bb cc dd` sentinel (part of the `.cfg`)          |
| `0x3fb` | 1    | pin-config version, must be `4` (part of the `.cfg`) |
| `0x3fc` | 4    | `crc32(cfg[0:0x80])` – standard zlib CRC-32, LE      |

Flags A: bit0 TrackPoint middle-button scroll, bit1 double-tap TP middle =
Backspace, bit2 mouse middle = Backspace, bit3 keep LEDs in low-power mode,
bit4 disable Win key, bit5 TrackPoint on at power-up, bit6 invert TrackPoint
X/Y, bit7 allow low-power in USB mode.

Flags B: bit0 disable STA blinking, bit1 STA off when not blinking, bit2 swap
Fn/LCtrl, bit3 enable Bluetooth passkey, bit4 only specific keys wake.

Board types: 0 debug, 1 ThinkPad wireless, 2 mechanical wireless,
3 TrackPoint mechanical, 4 TrackPoint-60 mechanical, 5 ThinkPad wireless 2,
6 TrackPoint-60 mechanical 2, 7 ThinkPad wireless 3, 10 wired USB.

## `.cfg` files

These are text files with GBK-encoded comments. Everything after `;` is
ignored. Each `0xNN` token is one byte and each `0xNNNN` token is a
little-endian 16-bit word. A pin file parses to exactly 968 bytes and is placed
at offset `0x34`. A layer file parses to 2048 bytes ending in `aa bb cc dd`.
