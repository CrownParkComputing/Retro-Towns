#!/usr/bin/env bash
# Retro-Towns - Linux build.
#
# Two halves, because the estate has two halves: the core is a CMake project
# (upstream Tsugaru's own build system, which is not going to be rewritten),
# and the front end is a handful of .cpp files compiled directly, which is how
# Retro-PSX and Retro-Saturn do it on Linux. The link step is where they meet.
#
#   tool/build-linux.sh            build core + front end
#   tool/build-linux.sh core       build the core only
#   OUT=/tmp/x tool/build-linux.sh put the artifacts somewhere else
set -euo pipefail

APP="${APP:-/home/jon/StudioProjects/Retro-Towns}"
CORE="$APP/core"
OUT="${OUT:-/home/jon/.cache/retro-restructure/retrotowns-linux}"
JOBS="${JOBS:-3}"

IMGUI="${IMGUI:-/home/jon/StudioProjects/Retro-Saturn/core/vendor/imgui/imgui}"
STB="${STB:-/home/jon/StudioProjects/Retro-Saturn/core/vendor/stb/stb}"

pkg-config --exists sdl3 || { echo "error: sdl3 not found by pkg-config" >&2; exit 1; }
echo "==> SDL3 $(pkg-config --modversion sdl3)"

mkdir -p "$OUT"

# ---------------------------------------------------------------- core
echo "==> building the Tsugaru core"
cmake -S "$CORE" -B "$CORE/build" -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "$CORE/build" --target ftowns_bridge -j "$JOBS" > /dev/null
echo "    $(ls -lh "$CORE/build/retro/bridge/libftowns_bridge.a" | awk '{print $5}')  libftowns_bridge.a"

# ---------------------------------------------------------------- libchdr
# A .chd has to be decoded to a bin/cue before the core can mount it, and
# Tsugaru has no CHD reader.  Vendored from Retro-PSX, built as its own
# project so that nothing is added to Tsugaru's CMake tree.
echo "==> building libchdr"
cmake -S "$CORE/vendor/libchdr" -B "$CORE/build-chdr" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF > /dev/null
cmake --build "$CORE/build-chdr" -j "$JOBS" > /dev/null

if [ "${1:-all}" = "core" ]; then
    echo "==> core only, stopping here"
    exit 0
fi

# ---------------------------------------------------------------- front end
FE="$OUT/frontend"; mkdir -p "$FE"

CORE_VER="$(git -C "$CORE" describe --tags --always 2>/dev/null || echo 0.1.0)"
CORE_DATE="$(git -C "$CORE" log -1 --format=%cd --date=short 2>/dev/null || echo unknown)"
CORE_DESC="Tsugaru FM TOWNS core"
echo "==> core: Tsugaru $CORE_VER ($CORE_DESC, $CORE_DATE)"

CXXFLAGS=(-std=gnu++17 -O2 -g0 -fPIC -Wall -Wextra -Wno-unused-parameter)
# Quoted as an array, not a string: a value with a space in it ("Tsugaru FM
# TOWNS core") splits into four words on unquoted expansion and leaves the
# compiler an unterminated string literal.
DEFS=(-DTOWNS_CORE_VERSION="\"$CORE_VER\"" -DTOWNS_CORE_DESC="\"$CORE_DESC\"" -DTOWNS_CORE_DATE="\"$CORE_DATE\"")

# minizip is not only for RetroMedia: a zipped rip has to be unpacked before the
# core can mount it, so the front end needs it even with no artwork server.
MINIZIP_CFLAGS=(); MINIZIP_LIBS=()
if pkg-config --exists minizip; then
    DEFS+=(-DTOWNS_HAVE_MINIZIP=1)
    read -r -a MINIZIP_CFLAGS <<< "$(pkg-config --cflags minizip)"
    read -r -a MINIZIP_LIBS <<< "$(pkg-config --libs minizip)"
    echo "==> minizip $(pkg-config --modversion minizip) (zip rips)"
else
    echo "    note: no minizip - .zip rips will not unpack" >&2
fi

MEDIA_CFLAGS=(); MEDIA_LIBS=()
if pkg-config --exists libcurl && [ ${#MINIZIP_LIBS[@]} -gt 0 ]; then
    DEFS+=(-DTOWNS_MEDIA_HTTP=1)
    read -r -a MEDIA_CFLAGS <<< "$(pkg-config --cflags libcurl)"
    read -r -a MEDIA_LIBS <<< "$(pkg-config --libs libcurl)"
    echo "==> RetroMedia: libcurl $(pkg-config --modversion libcurl)"
fi

read -r -a SDL_CFLAGS <<< "$(pkg-config --cflags sdl3)"
read -r -a SDL_LIBS <<< "$(pkg-config --libs sdl3)"
INCS=(-I"$APP/frontend" -I"$CORE/retro/bridge" -I"$CORE/vendor/libchdr/include" -I"$STB" -I"$IMGUI" -I"$IMGUI/backends" "${SDL_CFLAGS[@]}" "${MINIZIP_CFLAGS[@]}" "${MEDIA_CFLAGS[@]}")
DEFS+=(-DTOWNS_HAVE_LIBCHDR=1)

OBJS=""
for src in "$APP"/frontend/*.cpp \
           "$IMGUI"/imgui.cpp "$IMGUI"/imgui_draw.cpp "$IMGUI"/imgui_tables.cpp \
           "$IMGUI"/imgui_widgets.cpp \
           "$IMGUI"/backends/imgui_impl_sdl3.cpp \
           "$IMGUI"/backends/imgui_impl_sdlrenderer3.cpp; do
    obj="$FE/$(basename "${src%.cpp}").o"
    g++ "${CXXFLAGS[@]}" "${DEFS[@]}" "${INCS[@]}" -c -o "$obj" "$src" \
        || { echo "error: compile failed on $src" >&2; exit 1; }
    OBJS="$OBJS $obj"
done

# Every archive the core build produced.  Tsugaru's targets reference each
# other in a cycle (towns -> outside_world -> towns), so the linker needs them
# all in one group rather than in an order anyone has to get right.
LIBS=()
while IFS= read -r a; do
    LIBS+=("$a")
done < <(find "$CORE/build" "$CORE/build-chdr" -name '*.a' | sort)

echo "==> linking retrotowns"
g++ -o "$OUT/retrotowns" $OBJS \
    -Wl,--start-group "${LIBS[@]}" -Wl,--end-group \
    "${SDL_LIBS[@]}" "${MINIZIP_LIBS[@]}" "${MEDIA_LIBS[@]}" -lm -ldl -lpthread

mkdir -p "$OUT/assets"
echo "==> $OUT/retrotowns"
