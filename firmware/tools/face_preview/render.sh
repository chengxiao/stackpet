#!/bin/bash
# Render every face style x emotion to PNG sheets (pixel-accurate LVGL software rendering, no device needed).
#   ./render.sh [output dir]      default: ./preview next to this script
# Set FACE_PREVIEW_BUILD to use a separate build dir (parallel agents).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="${FACE_PREVIEW_BUILD:-$HERE/build}"
OUT="${1:-$HERE/preview}"
# Optional wrapper that enters the ESP-IDF environment (the author's idf-env.sh next to the repository); without it
# the tools have to be on PATH already
ENV="$HERE/../../../../idf-env.sh"
if [ ! -x "$ENV" ]; then
    ENV="env"
fi
"$ENV" cmake -S "$HERE" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release > /dev/null
"$ENV" cmake --build "$BUILD" -j 8 2>&1 | grep -E 'error|warning: ' | grep -vE 'managed_components|smooth_ui_toolkit' || true
mkdir -p "$OUT"
"$BUILD/face_preview" "$OUT"
uv run -q --with pillow python "$HERE/compose.py" "$OUT"
