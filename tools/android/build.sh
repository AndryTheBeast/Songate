#!/usr/bin/env bash
# Songate for Android: the APK, from the same source/ (CMakeLists.txt's ANDROID part) with Gradle; everything it uses is in C:\Android.
#   bash tools/android/build.sh                 a dev build (GF_DEV) for the emulator and phones: android/app/build/outputs/apk/debug/
#   ABIS=x86_64 bash tools/android/build.sh     the emulator's only (faster)
#   DEV=0 bash tools/android/build.sh           a release build's code (signing comes later)
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
./gradlew.bat --no-daemon -q assembleDebug -Pdev="${DEV:-1}" -Pabis="${ABIS:-x86_64,arm64-v8a}"
ls -l app/build/outputs/apk/debug/*.apk
