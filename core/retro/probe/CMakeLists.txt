set(TARGET_NAME tsugaru_probe)

add_executable(${TARGET_NAME} probe.cpp)
set_target_properties(${TARGET_NAME} PROPERTIES OUTPUT_NAME probe)

target_link_libraries(${TARGET_NAME} ftowns_bridge headless_mode)
