#!/usr/bin/env python3
# Assemble the full 8MB factory flash image for the project and copy the OTA
# image. Run after `pio run -e safeboot -e esp32-s3`.
#
# Single-slot OTA layout (Tasmota-compatible, 8 MB):
#   Offset   | Content
#   --------+-----------------------------------------------------
#   0x1000   | bootloader                       (ESP32-S3: bootloader @0x0000)
#   0x0000   | bootloader
#   0x8000   | partition table
#   0xe000   | otadata -> selects app0/ota_0 (first boot goes straight to the app)
#   0x10000  | safeboot recovery firmware (factory)
#   0xE0000  | app0 (ota_0 = the display application - the OTA slot)
#   0x3B0000 | spiffs - blank (0xFF, formatted on first boot)
#
#   DIO flash mode, 40 MHz - QIO is forbidden on this module.
#
#   factory-8mb.img        esptool write_flash 0x0 factory-8mb.img
#   ota-esp32s3-8mb.bin    OTA image: installable from this project's WebUI
#                          (/up flow) and from the safeboot recovery UI.
import os
import shutil
import struct
import subprocess
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST = os.path.join(ROOT, "dist")
B = os.path.join(ROOT, ".pio", "build")

SPIFFS_OFF = 0x3B0000
SPIFFS_SIZE = 0x450000


def need(path):
    if not os.path.isfile(path):
        sys.exit("missing build artifact: " + path)
    return path


def make_otadata(seq):
    """A single OTA-partition boot selection, ESP-IDF 5.x / Tasmota compatible.

    Layout of a 32-byte esp_ota_select_entry_t that the 2nd-stage bootloader
    (bootloader_common_ota_select_valid) actually accepts:
        ota_seq   [0:4]    sequence number, 1-based
        seq_label [4:24]   0xFF (reserved)
        ota_state [24:28]  0xFFFFFFFF (ESP_OTA_IMG_UNDEFINED -> boot, app rollback disabled)
        crc       [28:32]  CRC32 (init 0xFFFFFFFF, no final xor, little-endian)
                           over the ota_seq field only
    An entry is valid iff ota_state is not INVALID/ABORTED and
    crc == bootloader_common_ota_select_crc(entry). When invalid, the
    bootloader falls back to the first APP partition (our factory/safeboot),
    so a malformed entry manifests as booting into L7-RECOVERY - not the app.
    """
    crc = zlib.crc32(struct.pack("<I", seq), 0xFFFFFFFF)
    entry = struct.pack("<I", seq) + b"\xff" * 20 + struct.pack("<II", 0xFFFFFFFF, crc)
    assert len(entry) == 32
    data = bytearray([0xFF] * 0x2000)
    data[0:32] = entry            # sector 0
    data[0x1000:0x1000 + 32] = entry  # sector 1 (both copies, same valid entry)
    return bytes(data)


def main():
    os.makedirs(DIST, exist_ok=True)

    bootloader = need(os.path.join(B, "esp32-s3", "bootloader.bin"))
    ptable = need(os.path.join(B, "esp32-s3", "partitions.bin"))
    stub = need(os.path.join(B, "safeboot", "firmware.bin"))
    app = need(os.path.join(B, "esp32-s3", "firmware.bin"))

    ota = os.path.join(DIST, "ota-esp32s3-8mb.bin")
    shutil.copy2(app, ota)
    print("OTA image: %s" % ota)

    fs_img = os.path.join(DIST, "spiffs_blank.bin")
    with open(fs_img, "wb") as f:
        f.write(b"\xff" * SPIFFS_SIZE)

    otadata = make_otadata(1)  # boot app0/ota_0 on first boot
    otadata_bin = os.path.join(DIST, "otadata_app0.bin")
    with open(otadata_bin, "wb") as f:
        f.write(otadata)

    img = os.path.join(DIST, "factory-8mb.img")
    subprocess.check_call([
        "esptool.py", "--chip", "esp32s3", "merge-bin",
        "-o", img,
        "--flash-mode", "dio",
        "--flash-freq", "40m",
        "--flash-size", "8MB",
        "0x0", bootloader,
        "0x8000", ptable,
        "0xe000", otadata_bin,
        "0x10000", stub,
        "0xE0000", app,
        "0x%X" % SPIFFS_OFF, fs_img,
    ])
    print("Factory image: %s" % img)
    print("Flash with: esptool.py --chip esp32s3 --before default-reset --after hard-reset \\")
    print("             write-flash --flash-mode dio --flash-freq 40m --flash-size 8MB 0x0 %s" % img)


if __name__ == "__main__":
    main()