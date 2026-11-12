#!/usr/bin/env bash
# Songate for Android: the APK, from the same source/ (CMakeLists.txt's ANDROID part) with Gradle; everything it uses is in C:\Android.
#   bash tools/android/build.sh                 a dev build (GF_DEV) for the emulator and phones: android/app/build/outputs/apk/debug/
#   ABIS=x86_64 bash tools/android/build.sh     the emulator's only (faster)
#   DEV=0 bash tools/android/build.sh           a release build's code, debug-signed (a check that it compiles)
#   RELEASE=1 bash tools/android/build.sh       the release: no dev extras, both ABIs, signed with Songate's key
#                                               (C:\Android\keys\songate-release.properties), as build-android/release/SongateAND-<version>.apk
# tools/android/build-deps.sh makes the libraries first.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
export JAVA_HOME="C:\\Android\\jdk-17"
export ANDROID_HOME="C:\\Android\\sdk"
export GRADLE_USER_HOME="C:\\Android\\gradle"   # Gradle's downloads and caches stay with the rest of the toolchain

printf 'sdk.dir=C\\:\\\\Android\\\\sdk\n' > "$ROOT/android/local.properties"
rm -rf "$ROOT/android/build/assets" && mkdir -p "$ROOT/android/build/assets"
cp -r "$ROOT/romfs" "$ROOT/android/build/assets/romfs"

cd "$ROOT/android"
if [ "${RELEASE:-0}" = "1" ]; then
  [ -f "${SONGATE_KEY_PROPERTIES:-/c/Android/keys/songate-release.properties}" ] || { echo "no signing key: C:\Android\keys\songate-release.properties is missing"; exit 1; }
  ./gradlew.bat --no-daemon -q assembleRelease -Pdev=0 -Pabis="x86_64,arm64-v8a"
  VERSION="$(grep -E '^APP_VERSION' "$ROOT/Makefile" | sed -E 's/^APP_VERSION[[:space:]:=]*//; s/[[:space:]]+$//')"
  # 2.0.0 BETA 1 → SongateAND-2.0.0-beta.1, the spelling of the tag (plat::isInstaller finds it by that start)
  NAME="SongateAND-$(echo "$VERSION" | tr 'A-Z' 'a-z' | sed -E 's/ ([a-z]+) ([0-9]+)$/-\1.\2/; s/ /-/g')"
  mkdir -p "$ROOT/build-android/release"
  cp app/build/outputs/apk/release/app-release.apk "$ROOT/build-android/release/$NAME.apk"
  echo "built $ROOT/build-android/release/$NAME.apk"
  exit 0
fi
./gradlew.bat --no-daemon -q assembleDebug -Pdev="${DEV:-1}" -Pabis="${ABIS:-x86_64,arm64-v8a}"
ls -l app/build/outputs/apk/debug/*.apk
