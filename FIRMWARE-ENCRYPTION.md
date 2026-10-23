# XYKBMS firmware encryption

The firmware images shipped with the vendor tool are encrypted. This note
records what the encryption is, how that was determined, and what would be
needed to recover the key. It is the result of static analysis of the files in
`TP Wired Keyboard Upgrade Program 230615/`; nothing here decrypts them.

## The images

| File | Size | Byte entropy |
|------|------|--------------|
| `xykbms_app_release.bin` | 40960 | 7.962 bits/byte |
| `xykbms_bootupdate_release.bin` | 30720 | 7.918 bits/byte |

`xykbms_usb_release.bin` is referenced by the Windows tool but is not in this
copy of the package.

The entropy is very close to the 8.0 bits/byte of random data, so the images
are either encrypted or compressed. The structure below shows they are
encrypted.

How the image is sent on the wire is in [PROTOCOL.md](PROTOCOL.md): the tool
sends the file contents in 16-byte blocks. That describes the transfer framing
only. No decryption step was found in the tool, which suggests the board's
bootloader decrypts the image while flashing and that the key is not in
`XYKBMS_SwUpdate.exe`, but neither point has been verified.

## It is probably a 64-bit block cipher in ECB mode

Several independent observations point to a 64-bit (8-byte) block cipher used
in ECB mode (each block encrypted on its own, with no IV or chaining). This is
evidence, not proof: the plaintext and any IV or chaining state are unknown,
and confirming it needs a known-plaintext test or the bootloader itself.

* **Repeating 8-byte blocks.** `xykbms_app_release.bin` ends with the same
  8-byte block, `3d 51 30 2e 31 62 1f f2`, repeated 235 times. This is most
  likely the encryption of a constant plaintext region, such as the
  erased-flash (`0xff`) or zero padding at the end of the image. A correctly
  used stream cipher or chained mode (CBC, CTR, …) would not normally repeat
  like this for constant input, so this is consistent with ECB and an 8-byte
  block size.

* **The block size is 8, not 16.** The 16-byte repeat unit in the padding is
  exactly that 8-byte block written twice (`a[-16:-8] == a[-8:]`). An AES-class
  128-bit cipher over constant input would repeat every 16 bytes without the two
  halves being equal, so this is a 64-bit cipher, not AES.

* **Same key for both images.** That identical padding block
  `3d 51 30 2e 31 62 1f f2` appears in the boot updater too (269 times). In
  addition, eight further 8-byte blocks are identical between the two files at
  the same offsets (`0x10`, `0x30`, `0xa8`, `0xb0`, `0xb8`, `0xd8`, `0xe0`,
  `0xe8`) — for example the block at offset `0x10` is `45 06 85 8a ab 21 5d f2`
  in both. Identical ciphertext for identical plaintext under the same key is
  the ECB signature, and it means both images are encrypted with the **same
  key** and share a common header region (magic/version/device-id constants).

* **Full diffusion within a block.** Per-byte-column entropy is uniform
  (~7.84 bits) and no two distinct blocks differ in only one byte, so each
  block is thoroughly mixed. This is a real block cipher, not a byte XOR or a
  substitution table — those would leave per-position structure.

## What was ruled out

A dictionary attack was run against the known plaintext (the padding block,
assumed to be `0xff`×8 or `0x00`×8):

* **Keys:** 26 638 distinct keys derived from 7 481 strings — every string in
  `XYKBMS_SwUpdate.exe` (ASCII and UTF-16), plus vendor/shop/firmware-date
  constants, in raw, upper, lower, MD5 and hex-decoded forms.
* **Ciphers:** DES, 3DES (2- and 3-key), Blowfish, CAST5, RC2, and TEA / XTEA
  (32- and 64-round) / XXTEA in both endiannesses.
* **Result:** 399 431 cipher+key combinations tried, **zero** produced the
  assumed plaintext from the padding block.

So no tested cipher and key candidate matched for the assumed padding
plaintext. A plain XOR or substitution cipher was already excluded by the
diffusion test above. IDEA, GOST 28147-89, fully custom ciphers and other
plaintext guesses were not tried, and remain open.

## Where the key is, and how it could be recovered

The decryption happens in the bootloader on the keyboard's microcontroller
(an STM32F1), so the key is stored in that chip's flash. Recovering it means
reading the chip, not attacking the files. On an STM32F1 this depends entirely
on the Readout Protection (RDP) level in the option bytes:

* **RDP level 0** — flash is readable over the SWD debug port. Dump it and the
  key (and the bootloader's decrypt routine, which reveals the exact cipher) come
  with it.
* **RDP level 1** — the debugger attaches but flash reads are blocked. Lowering
  protection to read it triggers a **mass erase**, which destroys the key. So
  level 1 is a dead end for key recovery.
* **RDP level 2** — SWD is permanently disabled; the port gives nothing.

The deciding question is therefore just the RDP level, and reading it is
non-destructive. With an SWD probe (ST-Link clone, or a Raspberry Pi Pico
running `debugprobe`) on the board's SWDIO/SWCLK/GND pads:

```sh
# FLASH_OBR is at 0x4002201C; bit 1 is the read-protection flag
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
  -c "init; mdw 0x4002201c; shutdown"
```

If the board is left at RDP 0 (common on low-cost hardware), dumping the flash
would yield the key, after which both images decrypt and the real pin table
could finally be read — which is what the mic-mute-LED investigation in
[../README.md](../README.md) could not resolve from the config interface alone.

## Checking the app image padding

This checks the padding block and block size in `xykbms_app_release.bin`. Run
it from `TP Wired Keyboard Upgrade Program 230615/`. It does not reproduce the
entropy figures, the boot image count or the comparison between the images.

```python
import collections
b = open("xykbms_app_release.bin", "rb").read()
blocks = [b[i:i+8] for i in range(0, len(b), 8)]
pad, n = collections.Counter(blocks).most_common(1)[0]
print(pad.hex(), n)          # 3d51302e31621ff2 235
print(b[-16:-8] == b[-8:])   # True  -> 8-byte block, not 16
```
