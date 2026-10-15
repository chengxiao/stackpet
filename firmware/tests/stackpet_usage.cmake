# StackPet usage host tests (included from tests/CMakeLists.txt): the launcher's screensaver, the clock and Claude's
# usage from the Mac (the snapshot, how it looks, the console line that sets it up)
set(STACKPET_USAGE_MAIN_DIR ${CMAKE_CURRENT_LIST_DIR}/../main)

add_executable(stackpet_usage_test
    ${CMAKE_CURRENT_LIST_DIR}/stackpet_usage_test.cpp
    ${STACKPET_USAGE_MAIN_DIR}/stackpet/usage/usage_snapshot.cpp
    ${STACKPET_USAGE_MAIN_DIR}/stackpet/usage/usage_look.cpp
    ${STACKPET_USAGE_MAIN_DIR}/stackpet/usage/usage_config.cpp
)
target_include_directories(stackpet_usage_test PRIVATE ${STACKPET_USAGE_MAIN_DIR} ${CMAKE_CURRENT_LIST_DIR}
                                                       ${CMAKE_CURRENT_LIST_DIR}/../components/ArduinoJson/src)
target_compile_features(stackpet_usage_test PRIVATE cxx_std_17)
target_compile_options(stackpet_usage_test PRIVATE -Wall -Wextra)
add_test(NAME stackpet_usage_test COMMAND $<TARGET_FILE:stackpet_usage_test>)
