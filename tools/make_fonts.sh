#!/bin/bash
# Regenerate the pre-converted UbuntuMono bitmap fonts (src/fonts/).
# Requirements: node + lv_font_conv on PATH (see project log 2026-09-24).
#
# TWO TRAPS that produced blank text in the past - do NOT change these
# without verifying on the display:
#   1. --bpp MUST be 1, 2, 4 or 8. LVGL 9.5 cannot decode bpp=3 (silent blank).
#   2. --no-compress MUST stay: LV_USE_FONT_COMPRESSED=0 in this project,
#      compressed bitmaps decode to nothing, silently.
# Sizes cover every grid layout the dashboard math can produce (1-3 rows,
# 1-4 cols) plus the fixed screens (18 status, 24 setup, 28 config, 32 OTA).
set -e
cd "$(dirname "$0")/.."
R="0x20-0x7E,0xB0,0xC1,0xC9,0xCD,0xD3,0xD6,0xDA,0xDC,0xDD,0xE1,0xE9,0xED,0xF3,0xF6,0xFA,0xFC,0xFD,0x150,0x151,0x160,0x161,0x170,0x171,0x2026"
for s in 18 19 22 24 28 32 33 36 42 44 46 52; do
  lv_font_conv --size $s --bpp 4 --format lvgl \
    --font tools/fonts/UbuntuMono-R.ttf -r $R \
    --no-kerning --no-compress --lv-font-name ubuntu_$s \
    -o src/fonts/ubuntu_$s.c
  sed -i 's|#include "lvgl/lvgl.h"|#include "lvgl.h"|' src/fonts/ubuntu_$s.c
done
echo "done: $(ls src/fonts/ubuntu_*.c | wc -l) fonts"
