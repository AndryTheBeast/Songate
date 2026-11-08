#!/usr/bin/env bash
# Songate for Android: builds the libraries into tools/android/deps/prefix/<abi> with the NDK (the same versions as the PC's,
# sources from tools/windows/deps/dl and tools/android/deps/dl). LGPL ones (FFmpeg, mpg123) are shared, so they stay replaceable.
# Run from devkitPro's MSYS2 bash, which has make and the shell FFmpeg's configure needs:
#   C:\devkitPro\msys2\usr\bin\bash.exe -lc "/c/Users/andre/Desktop/GateFold-NX/tools/android/build-deps.sh"
# ABIS="x86_64" for the emulator only; names as arguments rebuild only those libraries.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
DEPS="$HERE/deps"
DL="$DEPS/dl"
WINDL="$ROOT/tools/windows/deps/dl"
SRC="$DEPS/src"
API=29
NDK="/c/Android/sdk/ndk/27.2.12479018"
TC="$NDK/toolchains/llvm/prebuilt/windows-x86_64"
NINJA_DIR="$(ls -d "/c/Users/andre/AppData/Local/Microsoft/WinGet/Packages/Ninja-build.Ninja_"* | head -1)"
export PATH="/c/Program Files/CMake/bin:$NINJA_DIR:$TC/bin:$PATH"
mkdir -p "$SRC"
m() { cygpath -m "$1"; }

unpack() {   # unpack <tarball> <folder it unpacks to>
  # symlinks (macOS frameworks, a README) cannot be made here; everything else unpacks
  [ -d "$SRC/$2" ] || tar -xf "$1" -C "$SRC" 2>/dev/null || [ -d "$SRC/$2" ]
}

cm() {       # cm <name> <source folder> <cmake options...>
  local name="$1" from="$2"; shift 2
  rm -rf "$BLD/$name"
  cmake -S "$(m "$from")" -B "$(m "$BLD/$name")" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$(m "$NDK/build/cmake/android.toolchain.cmake")" -DANDROID_ABI="$ABI" -DANDROID_PLATFORM=$API \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF \
    -DCMAKE_INSTALL_PREFIX="$(m "$PREFIX")" -DCMAKE_PREFIX_PATH="$(m "$PREFIX")" -DCMAKE_FIND_ROOT_PATH="$(m "$PREFIX")" \
    -DCMAKE_INSTALL_LIBDIR=lib "$@" >"$BLD/$name.log" 2>&1 || { tail -30 "$BLD/$name.log"; return 1; }
  cmake --build "$(m "$BLD/$name")" >>"$BLD/$name.log" 2>&1 || { tail -40 "$BLD/$name.log"; return 1; }
  cmake --install "$(m "$BLD/$name")" >>"$BLD/$name.log" 2>&1
}

cc_lib() {    # cc_lib <name> <source folder> <library> <include flags> <files...>: no build system that suits; a short list of files
  local name="$1" from="$2" out="$3" inc="$4"; shift 4
  local triple; case "$ABI" in arm64-v8a) triple=aarch64-linux-android ;; x86_64) triple=x86_64-linux-android ;; esac
  rm -rf "$BLD/$name"; mkdir -p "$BLD/$name"
  local objs=()
  for f in "$@"; do
    clang.exe --target=$triple$API --sysroot="$(m "$TC/sysroot")" -O2 -fPIC -w $inc -c "$(m "$from/$f.c")" -o "$(m "$BLD/$name/$f.o")"
    objs+=("$(m "$BLD/$name/$f.o")")
  done
  llvm-ar.exe rcs "$(m "$PREFIX/lib/$out")" "${objs[@]}"
}
b_ogg() {
  unpack "$WINDL/ogg/libogg-1.3.5.tar.xz" libogg-1.3.5
  cm ogg "$SRC/libogg-1.3.5" -DINSTALL_DOCS=OFF
}
b_opus() {
  unpack "$WINDL/opus/opus-1.5.2.tar.gz" opus-1.5.2
  cm opus "$SRC/opus-1.5.2" -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF
}
b_opusfile() {   # four files, no http
  unpack "$WINDL/opusfile/opusfile-0.12.tar.gz" opusfile-0.12
  cc_lib opusfile "$SRC/opusfile-0.12/src" libopusfile.a \
    "-DOP_DISABLE_HTTP -I$(m "$PREFIX/include") -I$(m "$PREFIX/include/opus") -I$(m "$SRC/opusfile-0.12/include")" info internal opusfile stream
  cp "$SRC/opusfile-0.12/include/opusfile.h" "$PREFIX/include/opus/"
}
b_tremor() {
  [ -d "$SRC/tremor-master" ] || tar -xf "$WINDL/tremor/tremor-master.tar.gz" -C "$SRC"
  mkdir -p "$PREFIX/include/tremor"
  cc_lib tremor "$SRC/tremor-master" libvorbisidec.a "-include endian.h -I$(m "$PREFIX/include") -I$(m "$SRC/tremor-master")" \
    block codebook floor0 floor1 info mapping0 mdct registry res012 sharedbook synthesis vorbisfile window
  cp "$SRC/tremor-master/"{ivorbiscodec,ivorbisfile,config_types}.h "$PREFIX/include/tremor/" 2>/dev/null || cp "$SRC/tremor-master/"{ivorbiscodec,ivorbisfile}.h "$PREFIX/include/tremor/"
}
b_flac() {
  unpack "$WINDL/flac/flac-1.4.3.tar.xz" flac-1.4.3
  cm flac "$SRC/flac-1.4.3" -DBUILD_CXXLIBS=OFF -DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_DOCS=OFF -DINSTALL_MANPAGES=OFF -DWITH_OGG=OFF
}
b_mpg123() {   # LGPL: shared
  unpack "$WINDL/mpg123/mpg123-1.32.10.tar.bz2" mpg123-1.32.10
  cm mpg123 "$SRC/mpg123-1.32.10/ports/cmake" -DBUILD_PROGRAMS=OFF -DBUILD_LIBOUT123=OFF -DBUILD_SHARED_LIBS=ON
}
b_jpeg() {
  unpack "$WINDL/jpeg/libjpeg-turbo-3.0.4.tar.gz" libjpeg-turbo-3.0.4
  local simd=ON; [ "$ABI" = x86_64 ] && simd=OFF   # x86's SIMD wants nasm; the emulator does not need the speed
  cm jpeg "$SRC/libjpeg-turbo-3.0.4" -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=ON -DWITH_SIMD=$simd
}
b_mbedtls() {
  unpack "$WINDL/mbedtls/mbedtls-2.28.10.tar.bz2" mbedtls-2.28.10
  cm mbedtls "$SRC/mbedtls-2.28.10" -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
    -DUSE_STATIC_MBEDTLS_LIBRARY=ON -DMBEDTLS_FATAL_WARNINGS=OFF -DINSTALL_MBEDTLS_HEADERS=ON
}
b_curl() {     # Mbed TLS, as the console's; the system's certificates folder
  unpack "$WINDL/curl/curl-8.11.1.tar.xz" curl-8.11.1
  cm curl "$SRC/curl-8.11.1" -DBUILD_CURL_EXE=OFF -DBUILD_STATIC_LIBS=ON -DCURL_USE_MBEDTLS=ON -DHTTP_ONLY=ON -DCURL_DISABLE_LDAP=ON \
    -DCURL_USE_LIBPSL=OFF -DUSE_LIBIDN2=OFF -DUSE_NGHTTP2=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DCURL_USE_LIBSSH2=OFF \
    -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF -DCURL_ZLIB=ON \
    -DMBEDTLS_INCLUDE_DIRS="$(m "$PREFIX/include")" -DMBEDTLS_LIBRARY="$(m "$PREFIX/lib/libmbedtls.a")" \
    -DMBEDX509_LIBRARY="$(m "$PREFIX/lib/libmbedx509.a")" -DMBEDCRYPTO_LIBRARY="$(m "$PREFIX/lib/libmbedcrypto.a")" \
    -DCURL_CA_PATH=/system/etc/security/cacerts -DCURL_CA_BUNDLE=none
}
b_webp() {     # SDL_image's WebP covers
  unpack "$DL/webp/libwebp-1.3.2.tar.gz" libwebp-1.3.2
  cm webp "$SRC/libwebp-1.3.2" -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF -DWEBP_BUILD_GIF2WEBP=OFF \
    -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF
}
b_sdl() {      # shared: SDL's Java side loads libSDL2.so
  unpack "$DL/sdl/SDL2-2.28.5.tar.gz" SDL2-2.28.5
  # NDK r27 made ALooper_pollAll unavailable; SDL's later releases call pollOnce, the same for one poll with no timeout
  sed -i 's/ALooper_pollAll(0, NULL/ALooper_pollOnce(0, NULL/' "$SRC/SDL2-2.28.5/src/sensor/android/SDL_androidsensor.c"
  # AAudio: an output Android disconnects (Bluetooth to the speaker, headphones out) is opened again; 2.28.5's own span on it
  cp "$HERE/sdl-patches/SDL_aaudio.c" "$SRC/SDL2-2.28.5/src/audio/aaudio/SDL_aaudio.c"
  cm sdl "$SRC/SDL2-2.28.5" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF
}
b_ttf() {
  unpack "$DL/sdl/SDL2_ttf-2.22.0.tar.gz" SDL2_ttf-2.22.0
  cm ttf "$SRC/SDL2_ttf-2.22.0" -DSDL2TTF_VENDORED=ON -DSDL2TTF_HARFBUZZ=ON -DSDL2TTF_SAMPLES=OFF
}
b_image() {
  unpack "$DL/sdl/SDL2_image-2.8.2.tar.gz" SDL2_image-2.8.2
  cm image "$SRC/SDL2_image-2.8.2" -DSDL2IMAGE_VENDORED=OFF -DSDL2IMAGE_SAMPLES=OFF -DSDL2IMAGE_BACKEND_STB=ON \
    -DSDL2IMAGE_WEBP=ON -DSDL2IMAGE_AVIF=OFF -DSDL2IMAGE_TIF=OFF -DSDL2IMAGE_JXL=OFF -DSDL2IMAGE_DEPS_SHARED=OFF
}
b_mixer() {    # a fallback only: FFmpeg plays everything; SDL_mixer's built-in decoders, no more libraries
  unpack "$DL/sdl/SDL2_mixer-2.8.0.tar.gz" SDL2_mixer-2.8.0
  cm mixer "$SRC/SDL2_mixer-2.8.0" -DSDL2MIXER_VENDORED=OFF -DSDL2MIXER_SAMPLES=OFF -DSDL2MIXER_DEPS_SHARED=OFF \
    -DSDL2MIXER_FLAC=ON -DSDL2MIXER_FLAC_DRFLAC=ON -DSDL2MIXER_FLAC_LIBFLAC=OFF -DSDL2MIXER_MP3=ON -DSDL2MIXER_MP3_MINIMP3=ON \
    -DSDL2MIXER_MP3_MPG123=OFF -DSDL2MIXER_VORBIS=STB -DSDL2MIXER_OPUS=OFF -DSDL2MIXER_MOD=OFF -DSDL2MIXER_MIDI=OFF \
    -DSDL2MIXER_WAVPACK=OFF -DSDL2MIXER_GME=OFF -DSDL2MIXER_CMD=OFF
}
b_ffmpeg() {   # the console's version, audio only, LGPL, shared
  unpack "$WINDL/ffmpeg/ffmpeg-7.1.tar.xz" ffmpeg-7.1
  rm -rf "$BLD/ffmpeg"; mkdir -p "$BLD/ffmpeg"
  local arch="$ABI" asm=""
  case "$ABI" in
    arm64-v8a) arch=aarch64; TRIPLE=aarch64-linux-android ;;
    x86_64) asm="--disable-x86asm"; TRIPLE=x86_64-linux-android ;;
  esac
  # configure wants a compiler for this PC too: LLVM's clang with the MSVC headers and libraries, as the PC build has them
  local BT="/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools" KIT="/c/Program Files (x86)/Windows Kits/10"
  local MSVC SDK; MSVC="$(ls -d "$BT/VC/Tools/MSVC/"* | sort | tail -1)"; SDK="$(ls "$KIT/Include" | sort | tail -1)"
  export INCLUDE="$(cygpath -w "$MSVC/include");$(cygpath -w "$KIT/Include/$SDK/ucrt");$(cygpath -w "$KIT/Include/$SDK/um");$(cygpath -w "$KIT/Include/$SDK/shared")"
  export LIB="$(cygpath -w "$MSVC/lib/x64");$(cygpath -w "$KIT/Lib/$SDK/ucrt/x64");$(cygpath -w "$KIT/Lib/$SDK/um/x64")"
  local HOSTCC; HOSTCC="$(cygpath -u "$(cygpath -d "/c/Program Files/LLVM/bin/clang.exe")")"
  ( cd "$BLD/ffmpeg"
    "$SRC/ffmpeg-7.1/configure" --host-cc="$HOSTCC" --host-ld="$HOSTCC" --host-ldflags=-fuse-ld=lld --host-os=win64 --prefix="$(m "$PREFIX")" --target-os=android --arch=$arch --enable-cross-compile \
      --cc=clang.exe --cxx=clang++.exe --ld=clang.exe --ar=llvm-ar.exe --nm=llvm-nm.exe --ranlib=llvm-ranlib.exe --strip=llvm-strip.exe \
      --sysroot="$(m "$TC/sysroot")" --extra-cflags="--target=$TRIPLE$API -fPIC" --extra-ldflags="--target=$TRIPLE$API" \
      --enable-shared --disable-static --enable-pic $asm \
      --disable-programs --disable-doc --disable-debug --disable-autodetect --disable-avdevice --disable-avfilter --disable-swscale --disable-network \
      --disable-everything --enable-protocol=file \
      --enable-demuxer=mov,aac,mp3,flac,ogg,wav \
      --enable-parser=aac,mpegaudio,flac,vorbis,opus \
      --enable-decoder=aac,alac,mp3float,mp3,flac,opus,vorbis,'pcm_*' >"$BLD/ffmpeg.log" 2>&1 || { tail -30 "$BLD/ffmpeg.log"; tail -30 ffbuild/config.log; exit 1; }
    make -j8 >>"$BLD/ffmpeg.log" 2>&1 || { tail -40 "$BLD/ffmpeg.log"; exit 1; }
    make install >>"$BLD/ffmpeg.log" 2>&1
    mkdir -p "$PREFIX/share/ffmpeg"
    cp ffbuild/config.log "$PREFIX/share/ffmpeg/config.log" )
}

ALL=(ogg opus opusfile tremor flac mpg123 jpeg mbedtls curl webp sdl ttf image mixer ffmpeg)
for ABI in ${ABIS:-x86_64 arm64-v8a}; do
  PREFIX="$DEPS/prefix/$ABI"
  BLD="$DEPS/obj/$ABI"
  mkdir -p "$PREFIX" "$BLD"
  for n in "${@:-${ALL[@]}}"; do
    echo "=== $ABI $n"
    "b_$n"
  done
done
echo "all built into $DEPS/prefix"
