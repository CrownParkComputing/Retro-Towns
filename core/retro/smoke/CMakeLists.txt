set(TARGET_NAME tsugaru_smoke)

add_executable(${TARGET_NAME} smoke.cpp)
set_target_properties(${TARGET_NAME} PROPERTIES OUTPUT_NAME smoke)

target_link_libraries(${TARGET_NAME} ftowns_bridge)
