#!/usr/bin/env bash
# Retro-Towns - Android build: libretrotowns.so, the SDL3 + Dear ImGui front end
# with the whole Tsugaru core linked in.
#
#   ANDROID_ABI=arm64-v8a ./android/build-core.sh
#
# Shaped after Retro-Saturn's android/build-core.sh, with two differences that
# come from what this app actually is:
#
#   - No mbedTLS and no libcurl.  Retro-Towns fetches nothing; the BIOS is
#     user-supplied and there is no artwork client yet, so the TLS stack would
#     be a third of the binary for code that is never called.
#   - The core is a static archive, not a .so.  Tsugaru's targets reference one
#     another in a cycle (towns -> outside_world -> towns), so the link needs
#     every archive inside one --start-group rather than an order anyone has to
#     get right - which is what the Linux script does, and what this repeats.
#
# Everything in build/ is a downloaded or generated artifact; nothing here is
# committed.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP="$(cd "$HERE/.." && pwd)"
CORE="$APP/core"

ANDROID_ABI="${ANDROID_ABI:-arm64-v8a}"
# 28, and for the same reason the Gradle project gives: it is the floor the core
# configures at, not a preference.  A front end built for a newer API than the
# library it loads fails at run time rather than at build time, so both halves
# read the same number.
ANDROID_API="${ANDROID_API:-28}"
ANDROID_NDK="${ANDROID_NDK:-${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/28.2.13676358}}"
SDL3_TAG="${SDL3_TAG:-release-3.2.20}"
ZLIB_TAG="${ZLIB_TAG:-v1.3.1}"
JOBS="${JOBS:-$(nproc)}"

# ImGui is vendored in the Saturn tree rather than duplicated here, exactly as
# tool/build-linux.sh reads it from the same place.
IMGUI="${IMGUI:-$CORE/vendor/imgui/imgui}"
if [ ! -f "$IMGUI/imgui.cpp" ]; then
    IMGUI="${IMGUI_FALLBACK:-/home/jon/StudioProjects/Retro-Saturn/core/vendor/imgui/imgui}"
fi

OUT="$HERE/build/$ANDROID_ABI"
SDL3_SRC="$HERE/build/SDL"
SDL3_PREFIX="$OUT/sdl3"
ZLIB_SRC="$HERE/build/zlib"

case "$ANDROID_ABI" in
    arm64-v8a)   TRIPLE=aarch64-linux-android ;;
    x86_64)      TRIPLE=x86_64-linux-android ;;
    armeabi-v7a) TRIPLE=armv7a-linux-androideabi ;;
    *) echo "error: unsupported ANDROID_ABI '$ANDROID_ABI'" >&2; exit 1 ;;
esac

[ -d "$ANDROID_NDK" ] || { echo "error: no NDK at $ANDROID_NDK" >&2; exit 1; }
[ -f "$IMGUI/imgui.cpp" ] || { echo "error: no ImGui at $IMGUI" >&2; exit 1; }
[ -f "$CORE/retro/bridge/ftowns_bridge.h" ] || {
    echo "error: no core at $CORE" >&2; exit 1; }

TOOLCHAIN="$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64"
CXX="$TOOLCHAIN/bin/${TRIPLE}${ANDROID_API}-clang++"
CC="$TOOLCHAIN/bin/${TRIPLE}${ANDROID_API}-clang"
[ -x "$CXX" ] || { echo "error: no compiler at $CXX" >&2; exit 1; }

NDK_CMAKE_ARGS=(
    -G Ninja
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake"
    -DANDROID_ABI="$ANDROID_ABI"
    -DANDROID_PLATFORM="android-$ANDROID_API"
    -DCMAKE_BUILD_TYPE=Release
    # Every archive below ends up inside a shared library, and a non-PIC object
    # cannot go there: the link fails with "relocation R_AARCH64_... cannot be
    # used when making a shared object".  Not optional, and not something the
    # per-project CMakeLists is supposed to remember.
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
)

mkdir -p "$OUT"

# ---------------------------------------------------------------------------
# 1. SDL3 for Android
# ---------------------------------------------------------------------------
if [ ! -f "$SDL3_PREFIX/lib/libSDL3.so" ]; then
    echo "==> SDL3 ($SDL3_TAG) for $ANDROID_ABI"
    [ -d "$SDL3_SRC" ] || git clone --depth 1 --branch "$SDL3_TAG" \
        https://github.com/libsdl-org/SDL.git "$SDL3_SRC"
    cmake -S "$SDL3_SRC" -B "$OUT/sdl3-build" \
        "${NDK_CMAKE_ARGS[@]}" \
        -DCMAKE_INSTALL_PREFIX="$SDL3_PREFIX" \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF >/dev/null
    cmake --build "$OUT/sdl3-build" -j"$JOBS" >/dev/null
    cmake --install "$OUT/sdl3-build" >/dev/null
fi
echo "==> SDL3: $SDL3_PREFIX/lib/libSDL3.so"

# ---------------------------------------------------------------------------
# 2. zlib, for contrib/minizip
# ---------------------------------------------------------------------------
# libz itself is in the NDK sysroot and Android has always shipped it, so this
# clone is here for the two files that read a zip and nothing else.
if [ ! -d "$ZLIB_SRC" ]; then
    echo "==> zlib ($ZLIB_TAG), for contrib/minizip"
    git clone --depth 1 --branch "$ZLIB_TAG" \
        https://github.com/madler/zlib.git "$ZLIB_SRC"
fi

# ---------------------------------------------------------------------------
# 3. The Tsugaru core and its bridge
# ---------------------------------------------------------------------------
# The Android toolchain file is what decides the target; upstream's own SDL3 GUI
# is not added by the `ftowns_bridge` target, so building only that target leaves
# fsguilib, ysgl and fssimplewindow out of the link entirely.  That is the
# Milestone 1 assertion, and it holds here unchanged.
echo "==> the Tsugaru core"
# Quiet on success, but the tail of the log on failure: ninja writes compiler
# errors to stdout, so a plain >/dev/null hides exactly the line that matters.
cmake -S "$CORE" -B "$OUT/core" "${NDK_CMAKE_ARGS[@]}" > "$OUT/core-configure.log" 2>&1 || {
    tail -30 "$OUT/core-configure.log" >&2; exit 1; }
cmake --build "$OUT/core" -j"$JOBS" --target ftowns_bridge > "$OUT/core-build.log" 2>&1 || {
    grep -E 'error|FAILED' "$OUT/core-build.log" | tail -30 >&2; exit 1; }

# ---------------------------------------------------------------------------
# 4. libchdr
# ---------------------------------------------------------------------------
# A .chd has to be decoded to a bin/cue before the core can mount it, and
# Tsugaru has no CHD reader.  Vendored, built as its own project so that nothing
# is added to Tsugaru's CMake tree.  Its lzma, miniz and zstd come from its own
# deps directory, which is why this cross-compiles without hunting the sysroot.
echo "==> libchdr"
cmake -S "$CORE/vendor/libchdr" -B "$OUT/chdr" "${NDK_CMAKE_ARGS[@]}" \
    -DBUILD_SHARED_LIBS=OFF >/dev/null
cmake --build "$OUT/chdr" -j"$JOBS" >/dev/null

# ---------------------------------------------------------------------------
# 5. The front end
# ---------------------------------------------------------------------------
CORE_VER="$(git -C "$CORE" describe --tags --always 2>/dev/null || echo 0.1.0)"
CORE_DATE="$(git -C "$CORE" log -1 --format=%cd --date=short 2>/dev/null || echo unknown)"
CORE_DESC="Tsugaru FM TOWNS core"
echo "==> front end (Tsugaru $CORE_VER, $CORE_DATE)"

FE_OUT="$OUT/frontend"; mkdir -p "$FE_OUT"
FE_OBJS=""
# Quoted as an array, not a string: "Tsugaru FM TOWNS core" splits into four
# words on unquoted expansion and leaves the compiler an unterminated literal.
DEFS=(-DANDROID -DTOWNS_HAVE_MINIZIP=1 -DTOWNS_HAVE_LIBCHDR=1
      -DTOWNS_CORE_VERSION="\"$CORE_VER\""
      -DTOWNS_CORE_DESC="\"$CORE_DESC\""
      -DTOWNS_CORE_DATE="\"$CORE_DATE\"")
INCS=(-I"$APP/frontend" -I"$CORE/retro/bridge"
      -I"$CORE/vendor/libchdr/include"
      -I"$IMGUI" -I"$IMGUI/backends" -I"$SDL3_PREFIX/include"
      -I"$ZLIB_SRC/contrib" -I"$ZLIB_SRC")

for src in "$APP"/frontend/*.cpp \
           "$IMGUI"/imgui.cpp "$IMGUI"/imgui_draw.cpp "$IMGUI"/imgui_tables.cpp \
           "$IMGUI"/imgui_widgets.cpp \
           "$IMGUI"/backends/imgui_impl_sdl3.cpp \
           "$IMGUI"/backends/imgui_impl_sdlrenderer3.cpp; do
    obj="$FE_OUT/$(basename "${src%.cpp}").o"
    "$CXX" -std=gnu++17 -O2 -g0 -fPIC -Wall -Wextra -Wno-unused-parameter \
        "${DEFS[@]}" "${INCS[@]}" -c -o "$obj" "$src" || {
        echo "error: compile failed on $src" >&2; exit 1; }
    FE_OBJS="$FE_OBJS $obj"
done

echo "==> minizip"
MZ_OBJS=""
for src in "$ZLIB_SRC"/contrib/minizip/unzip.c "$ZLIB_SRC"/contrib/minizip/ioapi.c; do
    obj="$FE_OUT/$(basename "${src%.c}").o"
    "$CC" -O2 -g0 -fPIC -I"$ZLIB_SRC" -I"$ZLIB_SRC/contrib/minizip" \
        -c -o "$obj" "$src" || {
        echo "error: minizip compile failed on $src" >&2; exit 1; }
    MZ_OBJS="$MZ_OBJS $obj"
done

# ---------------------------------------------------------------------------
# 6. Link
# ---------------------------------------------------------------------------
LIBS=()
while IFS= read -r a; do
    LIBS+=("$a")
done < <(find "$OUT/core" "$OUT/chdr" -name '*.a' | sort)
echo "==> linking libretrotowns.so ($((${#LIBS[@]})) archives)"

# --no-undefined turns "the core needs a symbol Android does not provide" into a
# build failure.  Without it the library links, installs, and dies in
# System.loadLibrary on a device, which is a much worse conversation.
# shellcheck disable=SC2086
"$CXX" -shared -fPIC -o "$OUT/libretrotowns.so" \
    $FE_OBJS $MZ_OBJS \
    -Wl,--start-group "${LIBS[@]}" -Wl,--end-group \
    -L"$SDL3_PREFIX/lib" -lSDL3 \
    -lz -lm -ldl -llog -landroid \
    -Wl,--no-undefined

# ---------------------------------------------------------------------------
# 7. Prove SDL_main is actually there
# ---------------------------------------------------------------------------
# SDLActivity looks the entry point up by name.  Without <SDL3/SDL_main.h> the
# front end's main() keeps its own name, the library loads, and the app dies at
# startup - a device-only mystery that this turns into a build error.
"$TOOLCHAIN/bin/llvm-nm" -D --defined-only "$OUT/libretrotowns.so" > "$OUT/exported.syms"
grep -q " SDL_main\$" "$OUT/exported.syms" || {
    echo "error: libretrotowns.so does not export SDL_main" >&2
    echo "       towns_app.cpp must include <SDL3/SDL_main.h>" >&2
    exit 1; }

# libc++_shared is a real runtime dependency, not an optional extra: leaving it
# out fails at System.loadLibrary time rather than at build time.
cp -f "$SDL3_PREFIX/lib/libSDL3.so" "$OUT/"
CXX_SHARED="$TOOLCHAIN/sysroot/usr/lib/$TRIPLE/libc++_shared.so"
[ -f "$CXX_SHARED" ] || CXX_SHARED="$(find "$TOOLCHAIN" -name libc++_shared.so -path "*$TRIPLE*" | head -1)"
[ -f "$CXX_SHARED" ] && cp -f "$CXX_SHARED" "$OUT/" || echo "warning: no libc++_shared.so" >&2

find "$OUT/sdl3-build" -name 'SDL3*.jar' ! -name '*sources*' \
    -exec cp -f {} "$OUT/SDL3.jar" \; 2>/dev/null || true

# ---------------------------------------------------------------------------
# 8. Install where Gradle actually looks
# ---------------------------------------------------------------------------
# Not a convenience.  Gradle packages app/src/main/jniLibs, and its
# mergeReleaseNativeLibs task reports UP-TO-DATE when nothing there changed - so
# making this a manual step produces a BUILD SUCCESSFUL that ships the previous
# library with none of the new code in it.
JNI_LIBS="$HERE/app/src/main/jniLibs/$ANDROID_ABI"
mkdir -p "$JNI_LIBS"
cp -f "$OUT/libretrotowns.so" "$OUT/libSDL3.so" "$JNI_LIBS/"
[ -f "$OUT/libc++_shared.so" ] && cp -f "$OUT/libc++_shared.so" "$JNI_LIBS/"

# Stripped, and only the copies being packaged - the ones in build/ keep their
# symbols so a native crash from a test build can still be read.  The Tsugaru
# core is far bigger with debug info than without it, and that difference is the
# download every user pays for.
for so in "$JNI_LIBS"/*.so; do
    "$TOOLCHAIN/bin/llvm-strip" --strip-unneeded "$so" 2>/dev/null || true
done

APP_LIBS="$HERE/app/libs"; mkdir -p "$APP_LIBS"
[ -f "$OUT/SDL3.jar" ] && cp -f "$OUT/SDL3.jar" "$APP_LIBS/"

echo "==> installed for packaging:"
ls -lh "$JNI_LIBS"/*.so
