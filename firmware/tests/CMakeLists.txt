cmake_minimum_required(VERSION 3.16)

project(stackchan_firmware_tests LANGUAGES CXX)

enable_testing()

add_executable(motion_math_test
    motion_math_test.cpp
    ../main/stackchan/motion/motion_math.cpp
)

target_include_directories(motion_math_test PRIVATE
    ../main
)

target_compile_features(motion_math_test PRIVATE cxx_std_17)

add_test(NAME motion_math_test COMMAND $<TARGET_FILE:motion_math_test>)

# StackPet host tests: each track owns its own include file so tracks never edit the same CMake file
foreach(stackpet_tests_file stackpet_games.cmake stackpet_web.cmake stackpet_audio.cmake stackpet_radar.cmake
                         stackpet_remote.cmake stackpet_usage.cmake)
    if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/${stackpet_tests_file})
        include(${CMAKE_CURRENT_LIST_DIR}/${stackpet_tests_file})
    endif()
endforeach()
