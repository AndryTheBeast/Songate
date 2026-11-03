#!/usr/bin/env bash
# Songate: builds tools/preview/out/seekvsskip.exe, a file's moment reached by seek and by exact skip, compared (see seekvsskip.cpp).
# Usage: FFMPEG=tools/preview/deps/ffmpeg bash tools/preview/build-seekvsskip.sh [<out-folder>]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/out}"
SRC="$HERE/../../source"
FFMPEG="${FFMPEG:?an FFmpeg win64 shared build folder}"
mkdir -p "$OUT"
FLAGS=(-std=c++20 -O2 -DGF_PREVIEW -D_CRT_SECURE_NO_WARNINGS -DNOMINMAX -DGF_FFMPEG -I"$SRC" -I"$FFMPEG/include")
clang++ "${FLAGS[@]}" "$HERE/seekvsskip.cpp" "$SRC/ffdecoder.cpp" \
  -L"$FFMPEG/lib" -lavformat -lavcodec -lswresample -lavutil -Xlinker /subsystem:console -o "$OUT/seekvsskip.exe"
cp "$FFMPEG/bin/"*.dll "$OUT/"
echo "built $OUT/seekvsskip.exe"
