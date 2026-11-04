# Songate for PC and Android. The Switch builds with the Makefile; this builds the same source/ for Windows and Android.
#   PC: tools/windows/build-deps.sh makes the libraries first, tools/windows/build.sh runs this.
#   Android: tools/android/build-deps.sh, then tools/android/build.sh (Gradle runs this with the NDK).
cmake_minimum_required(VERSION 3.25)

if(DEFINED ANDROID_ABI)   # (ANDROID itself is set only once project() reads the NDK's toolchain file)
  project(Songate CXX C)
  set(DEPS ${CMAKE_SOURCE_DIR}/tools/android/deps/prefix/${ANDROID_ABI})
  option(GF_DEV "A dev build: the frame times, the join trace" ON)
  file(STRINGS ${CMAKE_SOURCE_DIR}/Makefile VERSION_LINE REGEX "^APP_VERSION")
  string(REGEX REPLACE "^APP_VERSION[ \t:=]*" "" APP_VERSION "${VERSION_LINE}")

  # SDL2, FFmpeg and mpg123 are shared (SDL's Java side loads SDL2; the LGPL ones stay replaceable): packaged beside libmain
  foreach(lib SDL2 avformat avcodec swresample avutil mpg123)
    add_library(${lib} SHARED IMPORTED)
    set_target_properties(${lib} PROPERTIES IMPORTED_LOCATION ${DEPS}/lib/lib${lib}.so)
  endforeach()

  file(GLOB APP_SOURCES CONFIGURE_DEPENDS source/*.cpp)
  add_library(main SHARED
    ${APP_SOURCES}
    source/qrcodegen.c
    source/monocypher.c
    platform/android/platform_android.cpp
    platform/none/bgclient_none.cpp)
  target_compile_features(main PRIVATE cxx_std_20)
  target_compile_options(main PRIVATE -O2 -Wno-deprecated-declarations)
  target_compile_definitions(main PRIVATE
    GF_WINDOWED GF_ANDROID GF_FFMPEG GF_CURL GF_TURBOJPEG GF_AIRPLAY "GF_VERSION=\"${APP_VERSION}\"")
  if(GF_DEV)
    target_compile_definitions(main PRIVATE GF_DEV)
  endif()
  target_include_directories(main PRIVATE source sysmodule/include ${DEPS}/include ${DEPS}/include/opus)
  target_link_libraries(main PRIVATE
    ${DEPS}/lib/libSDL2_ttf.a ${DEPS}/lib/libSDL2_image.a ${DEPS}/lib/libSDL2_mixer.a
    ${DEPS}/lib/libharfbuzz.a ${DEPS}/lib/libfreetype.a
    ${DEPS}/lib/libwebpdemux.a ${DEPS}/lib/libwebp.a ${DEPS}/lib/libsharpyuv.a ${DEPS}/lib/libcpufeatures-webp.a
    ${DEPS}/lib/libFLAC.a ${DEPS}/lib/libopusfile.a ${DEPS}/lib/libopus.a ${DEPS}/lib/libvorbisidec.a ${DEPS}/lib/libogg.a
    ${DEPS}/lib/libturbojpeg.a
    ${DEPS}/lib/libcurl.a ${DEPS}/lib/libmbedtls.a ${DEPS}/lib/libmbedx509.a ${DEPS}/lib/libmbedcrypto.a
    SDL2 avformat avcodec swresample avutil mpg123
    z log android OpenSLES GLESv2 m)
  return()
endif()

project(Songate CXX C RC)

set(DEPS ${CMAKE_SOURCE_DIR}/tools/windows/deps/prefix)
set(SDL ${CMAKE_SOURCE_DIR}/tools/preview/deps/sdl)
option(GF_DEV "A dev build: the frame times, the join trace, the read-only folder" ON)

file(STRINGS ${CMAKE_SOURCE_DIR}/Makefile VERSION_LINE REGEX "^APP_VERSION")
string(REGEX REPLACE "^APP_VERSION[ \t:=]*" "" APP_VERSION "${VERSION_LINE}")

# <SDL2/SDL.h>, as the console's portlibs have them
set(SDLINC ${CMAKE_BINARY_DIR}/include)
foreach(p SDL2-2.28.5 SDL2_ttf-2.22.0 SDL2_image-2.8.2 SDL2_mixer-2.8.0)
  file(GLOB h ${SDL}/${p}/include/*.h)
  file(COPY ${h} DESTINATION ${SDLINC}/SDL2)
  list(APPEND SDLLIB ${SDL}/${p}/lib/x64)
endforeach()

file(GLOB APP_SOURCES CONFIGURE_DEPENDS source/*.cpp)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)   # the C library inside the program: no redistributable to install
add_executable(Songate
  ${APP_SOURCES}
  source/qrcodegen.c
  source/monocypher.c
  platform/windows/platform_windows.cpp
  platform/windows/winfiles.cpp
  platform/windows/winshell.cpp
  platform/none/bgclient_none.cpp
  platform/windows/songate.rc
  platform/windows/songate.manifest)

set_source_files_properties(platform/windows/songate.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_SOURCE_DIR}/platform/windows/songate.ico)
target_compile_features(Songate PRIVATE cxx_std_20)
target_compile_options(Songate PRIVATE
  -O2 -Wno-deprecated-declarations
  "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${CMAKE_SOURCE_DIR}/platform/windows/winshim.h>")
target_compile_definitions(Songate PRIVATE
  GF_WINDOWED GF_FFMPEG GF_CURL GF_TURBOJPEG GF_AIRPLAY "GF_VERSION=\"${APP_VERSION}\""
  _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS NOMINMAX UNICODE _UNICODE
  __PRFCHWINTRIN_H CURL_STATICLIB FLAC__NO_DLL)
if(GF_DEV)
  target_compile_definitions(Songate PRIVATE GF_DEV)
endif()
target_include_directories(Songate PRIVATE
  platform/windows/include platform/windows source sysmodule/include
  ${SDLINC} ${DEPS}/include ${DEPS}/include/opus)
target_link_directories(Songate PRIVATE ${SDLLIB} ${DEPS}/lib ${DEPS}/bin)
target_link_libraries(Songate PRIVATE
  SDL2 SDL2main SDL2_ttf SDL2_image SDL2_mixer
  avformat avcodec swresample avutil
  FLAC mpg123 opusfile opus vorbisidec ogg turbojpeg-static libcurl z mbedcrypto
  ws2_32 winmm shell32 ole32 uuid powrprof comctl32 dwmapi shcore runtimeobject crypt32 secur32 bcrypt iphlpapi shlwapi version dnsapi)
target_link_options(Songate PRIVATE "LINKER:/subsystem:$<IF:$<BOOL:${GF_DEV}>,console,windows>")
