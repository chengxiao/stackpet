#!/bin/bash
# Regenerate the StackPet title fonts (main/stackpet/ui/fonts/font_stackpet_title_{28,44}.c).
#
# The fonts only contain printable ASCII plus the CJK characters / full-width punctuation that appear in the
# StackPet sources, so re-run this script after adding or changing Chinese UI strings:
#   ./tools/gen_stackpet_fonts.sh
# Glyphs come from Alibaba PuHuiTi (the font the rest of the firmware uses); the few characters PuHuiTi's
# subset lacks (e.g. 蛳) are taken from Noto Sans (noto-qwen.ttf), both shipped with the xiaozhi-fonts component.
# Requires node/npx (lv_font_conv is fetched by npx) and a previous build (for managed_components/).
set -euo pipefail

FW_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$FW_DIR"
# Relative paths on purpose: lv_font_conv writes its command line into the generated file header
FONTS_DIR="managed_components/78__xiaozhi-fonts/ttf"
PRIMARY_TTF="$FONTS_DIR/puhui-common.ttf"
FALLBACK_TTF="$FONTS_DIR/noto-qwen.ttf"
OUT_DIR="main/stackpet/ui/fonts"
LV_FONT_CONV="lv_font_conv@1.5.2"
SOURCES=(main/stackpet main/apps/app_remote main/apps/app_freeze main/apps/app_spin main/apps/app_face main/apps/app_radar)

for TTF in "$PRIMARY_TTF" "$FALLBACK_TTF"; do
    if [ ! -f "$TTF" ]; then
        echo "error: $TTF not found, run idf.py build once so the component manager fetches xiaozhi-fonts" >&2
        exit 1
    fi
done

count() {
    printf '%s' "$1" | python3 -I -c 'import sys; print(len(sys.stdin.read()))'
}

PRIMARY_SYMBOLS="$(python3 -I tools/collect_cjk.py --in-font "$PRIMARY_TTF" "${SOURCES[@]}")"
MISSING_SYMBOLS="$(python3 -I tools/collect_cjk.py --not-in-font "$PRIMARY_TTF" "${SOURCES[@]}")"
FALLBACK_SYMBOLS="$(python3 -I - "$FALLBACK_TTF" "$MISSING_SYMBOLS" <<'EOF'
import sys
sys.path.insert(0, 'tools')
from collect_cjk import font_codepoints
covered = font_codepoints(sys.argv[1])
sys.stdout.write(''.join(c for c in sys.argv[2] if ord(c) in covered))
EOF
)"
UNAVAILABLE="$(python3 -I -c 'import sys; print("".join(c for c in sys.argv[1] if c not in sys.argv[2]))' "$MISSING_SYMBOLS" "$FALLBACK_SYMBOLS")"

echo "glyphs from PuHuiTi: $(count "$PRIMARY_SYMBOLS"), from Noto: $(count "$FALLBACK_SYMBOLS") ${FALLBACK_SYMBOLS}"
if [ -n "$UNAVAILABLE" ]; then
    echo "warning: no font has these characters, they will render as boxes: $UNAVAILABLE" >&2
fi

mkdir -p "$OUT_DIR"
for SIZE in 28 44; do
    OUT="$OUT_DIR/font_stackpet_title_${SIZE}.c"
    ARGS=(--font "$PRIMARY_TTF" --range 0x20-0x7E)
    if [ -n "$PRIMARY_SYMBOLS" ]; then
        ARGS+=(--symbols "$PRIMARY_SYMBOLS")
    fi
    if [ -n "$FALLBACK_SYMBOLS" ]; then
        ARGS+=(--font "$FALLBACK_TTF" --symbols "$FALLBACK_SYMBOLS")
    fi
    npx -y "$LV_FONT_CONV" "${ARGS[@]}" \
        --size "$SIZE" --bpp 4 --format lvgl --lv-include lvgl.h \
        --no-compress --no-prefilter --force-fast-kern-format \
        -o "$OUT" | grep -v 'Forced faster kerning format' || true
    echo "generated $OUT ($(wc -c < "$OUT" | tr -d ' ') bytes of C)"
done
