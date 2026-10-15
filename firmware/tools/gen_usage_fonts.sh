#!/bin/bash
# Regenerate the screensaver's fonts (main/stackpet/usage/fonts/font_usage_*.c) from Space Grotesk Bold (the digits
# of ClaudeUsageBar's own display) and Share Tech Mono (the terminal style), both under the SIL Open Font License 1.1:
#   SPACE_GROTESK_TTF=/path/to/SpaceGrotesk-Bold.ttf SHARE_TECH_MONO_TTF=/path/to/ShareTechMono-Regular.ttf \
#       ./tools/gen_usage_fonts.sh
# Only the digits and a few signs are in them (and printable ASCII in the small mono one). The generated fonts are
# not named after the fonts (Share Tech Mono reserves its name). Requires node/npx (lv_font_conv is fetched by npx).
set -euo pipefail

FW_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$FW_DIR"
TTF_IN="${SPACE_GROTESK_TTF:?set SPACE_GROTESK_TTF to a Space Grotesk Bold (700) TTF}"
MONO_IN="${SHARE_TECH_MONO_TTF:?set SHARE_TECH_MONO_TTF to a Share Tech Mono TTF}"
# Copied under build/ first, used by a relative path: lv_font_conv writes its command line into the generated file
WORK="build/usage_fonts"
OUT_DIR="main/stackpet/usage/fonts"
mkdir -p "$WORK" "$OUT_DIR"
cp "$TTF_IN" "$WORK/SpaceGrotesk-Bold.ttf"
cp "$MONO_IN" "$WORK/mono.ttf"

SG_NOTICE="/* Glyphs from Space Grotesk Bold: Copyright 2020 The Space Grotesk Project Authors
 * (https://github.com/floriankarsten/space-grotesk), under the SIL Open Font License 1.1
 * (https://scripts.sil.org/OFL) */"
MONO_NOTICE="/* Glyphs from Share Tech Mono: Copyright (c) 2012, Carrois Type Design, Ralph du Carrois, with Reserved Font
 * Name 'Share', under the SIL Open Font License 1.1 (https://scripts.sil.org/OFL) */"

gen() {  # name ttf size notice (symbols|range) value
    npx -y lv_font_conv@1.5.2 --font "$WORK/$2" "$5" "$6" \
        --size "$3" --bpp 4 --format lvgl --lv-include lvgl.h \
        --no-compress --no-prefilter --force-fast-kern-format \
        -o "$OUT_DIR/$1.c" | grep -v 'Forced faster kerning format' || true
    # The font's notice (its name table) on top
    { echo "$4"; cat "$OUT_DIR/$1.c"; } > "$WORK/$1.c" && mv "$WORK/$1.c" "$OUT_DIR/$1.c"
    echo "generated $OUT_DIR/$1.c ($(wc -c < "$OUT_DIR/$1.c" | tr -d ' ') bytes of C)"
}
gen font_usage_clock_96 SpaceGrotesk-Bold.ttf 96 "$SG_NOTICE" --symbols "0123456789:-"
gen font_usage_clock_52 SpaceGrotesk-Bold.ttf 52 "$SG_NOTICE" --symbols "0123456789:-"
gen font_usage_num_26 SpaceGrotesk-Bold.ttf 26 "$SG_NOTICE" --symbols "0123456789%-"
gen font_usage_mono_72 mono.ttf 72 "$MONO_NOTICE" --symbols "0123456789:-"
gen font_usage_mono_16 mono.ttf 16 "$MONO_NOTICE" --range 0x20-0x7E
