# StackPet web track host tests (included from CMakeLists.txt)
add_executable(stackpet_web_utils_test
    ${CMAKE_CURRENT_LIST_DIR}/stackpet_web_utils_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../main/stackpet/web/web_utils.cpp
)
target_include_directories(stackpet_web_utils_test PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../main)
target_compile_features(stackpet_web_utils_test PRIVATE cxx_std_17)
add_test(NAME stackpet_web_utils_test COMMAND $<TARGET_FILE:stackpet_web_utils_test>)
