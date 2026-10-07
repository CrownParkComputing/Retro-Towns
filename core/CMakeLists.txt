cmake_minimum_required(VERSION 3.20)

# Retro-Towns core fork - tsugaru-sdl3
#
# Upstream Tsugaru has no build root above src/, so this file is an addition
# rather than a modification: src/ is left exactly as captainys wrote it, which
# is what keeps a rebase cheap.  What it does is assemble the subset of the
# upstream tree that an emulation core needs, and skip everything that is
# Tsugaru's own GUI - fssimplewindow, connect_fssimplewindow, uitxt, townstips
# and the Windows icon resource - none of which the bridge links against.
project(TSUGARU_SDL3 CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(UNIX)
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-unused-variable")
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-variable")
endif()

add_compile_definitions(YS_LITTLE_ENDIAN)
add_compile_definitions(YS_TWOS_COMPLEMENT)

# Referenced by upstream's own test definitions; harmless and keeps
# configure-time variable references from failing if a subtree is added later.
set(DISC_IMAGE_DIR "$ENV{HOME}/TownsISO")

# ---- upstream sources, emulation core only ----

add_subdirectory(src/osdependent)
add_subdirectory(src/cpputil)
add_subdirectory(src/lineparser)
add_subdirectory(src/miscutil)
add_subdirectory(src/cheapmath)

add_subdirectory(src/vmbase)
add_subdirectory(src/device)
add_subdirectory(src/cpu)
add_subdirectory(src/inout)
add_subdirectory(src/ramrom)
add_subdirectory(src/vgmrecorder)

add_subdirectory(src/ym2612)
add_subdirectory(src/rf5c68)
add_subdirectory(src/i8251)
add_subdirectory(src/i8251_to_socket)
add_subdirectory(src/i8253)
add_subdirectory(src/diskdrive)
add_subdirectory(src/customstick)

add_subdirectory(src/towns)

add_subdirectory(src/discimg)
add_subdirectory(src/diskimg)

# townscommand and townsargv live under main_cui, which also builds
# Tsugaru_CUI and a test suite that wants a ROM checkout - so the two
# libraries are added directly and the CUI target never enters the build.
add_subdirectory(src/main_cui/command)
add_subdirectory(src/main_cui/argv)

# The whole of src/externals drags in fssimplewindow and the window/glue
# connection.  Only these three are core dependencies: the no-window sound
# library, the D77 disc format, and the PNG writer townssound's recorder uses.
add_subdirectory(src/externals/yssimplesound/src)
add_subdirectory(src/externals/d77)
add_subdirectory(src/externals/yspng)
add_subdirectory(src/externals/yssocket/src)

# ---- the Retro-Towns bridge ----

add_subdirectory(retro)
