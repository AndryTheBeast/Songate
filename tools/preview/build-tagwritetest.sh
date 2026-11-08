#!/usr/bin/env bash
# Songate · tagwritetest.exe (tagwritetest.cpp), with the app's own tag reader (source/metadata.cpp: mpg123 for MP3, libFLAC,
# FFmpeg for M4A) and the PC build's libraries (tools/windows/deps/prefix), so what is checked is what every device reads.
#   bash tools/preview/build-tagwritetest.sh tools/preview/deps/sdl
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/../.."
SDL="$1"
P="$ROOT/tools/windows/deps/prefix"
OUT="$HERE/out/tagwritetest"
mkdir -p "$OUT"
FLAGS=(-std=c++20 -O2 -DGF_FFMPEG -DSDL_MAIN_HANDLED -DFLAC__NO_DLL -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_WARNINGS -DNOMINMAX -D__PRFCHWINTRIN_H
       -Wno-deprecated-declarations -include "$ROOT/platform/windows/winshim.h" -I"$ROOT/platform/windows/include" -I"$ROOT/platform/windows" -I"$ROOT/sysmodule/include" -I"$P/include" -I"$P/include/opus" -I"$ROOT/source" -I"$SDL/SDL2-2.28.5/include" -I"$SDL/SDL2_image-2.8.2/include" -I"$HERE/out/include")
objs=()
for f in tagwrite metadata ffdecoder coverio; do
  clang++ "${FLAGS[@]}" -c "$ROOT/source/$f.cpp" -o "$OUT/$f.obj" &
  objs+=("$OUT/$f.obj")
done
clang++ "${FLAGS[@]}" -c "$HERE/tagwritetest.cpp" -o "$OUT/tagwritetest.obj" &
objs+=("$OUT/tagwritetest.obj")
clang++ "${FLAGS[@]}" -c "$HERE/tagwritetest_shim.cpp" -o "$OUT/tagwritetest_shim.obj" &
objs+=("$OUT/tagwritetest_shim.obj")
wait
clang++ "${objs[@]}" -L"$P/lib" -L"$P/bin" -L"$SDL/SDL2-2.28.5/lib/x64" -L"$SDL/SDL2_image-2.8.2/lib/x64" \
  -lFLAC -lmpg123 -lopusfile -lopus -lvorbisidec -logg -lavformat -lavcodec -lswresample -lavutil -lSDL2 -lSDL2_image \
  -lshell32 -lws2_32 -lwinmm -lbcrypt -lsecur32 -lole32 -luser32 -Xlinker /subsystem:console -o "$OUT/tagwritetest.exe"
cp "$P/bin/"*.dll "$OUT/" 2>/dev/null || true
cp "$SDL/SDL2-2.28.5/lib/x64/SDL2.dll" "$SDL/SDL2_image-2.8.2/lib/x64/SDL2_image.dll" "$OUT/"
echo "built $OUT/tagwritetest.exe"
