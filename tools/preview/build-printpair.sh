#!/usr/bin/env bash
# Songate: builds tools/preview/out/printpair.exe, listening together's fit of two files of one song, on the PC (see printpair.cpp).
# Usage: FFMPEG=tools/preview/deps/ffmpeg bash tools/preview/build-printpair.sh [<out-folder>]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/out}"
SRC="$HERE/../../source"
FFMPEG="${FFMPEG:?an FFmpeg win64 shared build folder}"
mkdir -p "$OUT"
FLAGS=(-std=c++20 -O2 -DGF_PREVIEW -D_CRT_SECURE_NO_WARNINGS -DNOMINMAX -DGF_FFMPEG -I"$SRC" -I"$FFMPEG/include")
clang++ "${FLAGS[@]}" "$HERE/printpair.cpp" "$SRC/together.cpp" "$SRC/ffdecoder.cpp" \
  -L"$FFMPEG/lib" -lavformat -lavcodec -lswresample -lavutil -Xlinker /subsystem:console -o "$OUT/printpair.exe"
cp "$FFMPEG/bin/"*.dll "$OUT/"
echo "built $OUT/printpair.exe"
