#!/bin/bash
# Render the launcher's screensaver (the clock and Claude's usage) to PNG: every scene in usage_preview.cpp, pixel
# accurate LVGL software rendering, no device needed.
#   ./render.sh [output dir] [snapshot.json]   default output: ./preview next to this script
# A snapshot (ClaudeUsageBar's GET /api/v1/snapshot) adds a scene with it. Set USAGE_PREVIEW_BUILD for another build
# dir. Requires node/npx (lv_font_conv) and a previous firmware build (managed_components/).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
FW="$(cd "$HERE/../.." && pwd)"
BUILD="${USAGE_PREVIEW_BUILD:-$HERE/build}"
OUT="${1:-$HERE/preview}"
SNAPSHOT="${2:-}"
# Optional wrapper that enters the ESP-IDF environment (the author's idf-env.sh next to the repository)
ENV="$HERE/../../../../idf-env.sh"
if [ ! -x "$ENV" ]; then
    ENV="env"
fi
mkdir -p "$BUILD" "$OUT"

# The CN 20 px font for the host: the TTF, size and depth of the firmware's (the assets partition's
# font_puhui_common_20_4), only the characters the screensaver uses
CN="$BUILD/font_preview_cn20.c"
SYMBOLS="$(cd "$FW" && python3 -I tools/collect_cjk.py main/stackpet/usage tools/usage_preview)"
(cd "$FW" && npx -y lv_font_conv@1.5.2 --font managed_components/78__xiaozhi-fonts/ttf/puhui-common.ttf \
    --range 0x20-0x7E --symbols "$SYMBOLS" --size 20 --bpp 4 --format lvgl --lv-include lvgl.h \
    --no-compress --no-prefilter --force-fast-kern-format -o "$CN") | grep -v 'Forced faster kerning format' || true

"$ENV" cmake -S "$HERE" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCN_FONT="$CN" > /dev/null
"$ENV" cmake --build "$BUILD" -j 8 2>&1 | grep -E 'error|warning: ' | grep -v managed_components || true
"$BUILD/usage_preview" "$OUT" ${SNAPSHOT:+"$SNAPSHOT"}
uv run -q --with pillow python "$HERE/compose.py" "$OUT"
