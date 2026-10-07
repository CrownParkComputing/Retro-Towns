set(TARGET_NAME ftowns_bridge)

add_library(${TARGET_NAME}
	${TARGET_NAME}.h
	${TARGET_NAME}.cpp
)

target_include_directories(${TARGET_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# The full list of what the bridge actually references.  Nothing in here pulls
# fssimplewindow, ysgl or fsguilib - if the build ever gains a GL or X11
# dependency it will be because something was added to this list, and the
# retro/smoke link check is what catches it.
target_link_libraries(${TARGET_NAME}
	towns
	townscommand
	townsargv
	townsrender
	outside_world
	discimg
	diskimg
	cpputil
	cpu
	lineParser
	miscutil
	ym2612
	rf5c68
	customstick
	d77
	yssimplesound_nownd
)
