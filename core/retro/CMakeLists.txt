add_subdirectory(bridge)

if(NOT ANDROID)
	add_subdirectory(smoke)
endif()

# Headless, so it builds and runs on both hosts - which is the whole point of
# having it: it is how a trace from an Android device is compared with one from
# a desktop.
add_subdirectory(probe)
