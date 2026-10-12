# StackPet RADAR host tests (included from tests/CMakeLists.txt): the pure C++ radar logic, the face tracker, the
# RGB565 converter of the FrameGrabber and the radar sound effects
set(STACKPET_RADAR_MAIN_DIR ${CMAKE_CURRENT_LIST_DIR}/../main)

function(stackpet_radar_add_test name)
    add_executable(${name} ${CMAKE_CURRENT_LIST_DIR}/${name}.cpp ${ARGN})
    target_include_directories(${name} PRIVATE ${STACKPET_RADAR_MAIN_DIR} ${CMAKE_CURRENT_LIST_DIR})
    target_compile_features(${name} PRIVATE cxx_std_17)
    target_compile_options(${name} PRIVATE -Wall -Wextra)
    add_test(NAME ${name} COMMAND $<TARGET_FILE:${name}>)
endfunction()

stackpet_radar_add_test(stackpet_radar_logic_test
    ${STACKPET_RADAR_MAIN_DIR}/apps/app_radar/radar_logic.cpp
)

# face_detector.cpp only compiles its pure part (FaceTracker) without ESP_PLATFORM
stackpet_radar_add_test(stackpet_radar_vision_test
    ${STACKPET_RADAR_MAIN_DIR}/stackpet/vision/face_detector.cpp
    ${STACKPET_RADAR_MAIN_DIR}/stackpet/sound/tone_synth.cpp
)

# The yaw servo's wheel mode entry: stackchan/motion/servo.cpp on the smooth_ui_toolkit core, with a host clock
# (stubs/hal/hal.h stands in for main/hal/hal.h)
set(STACKPET_UITK_DIR ${CMAKE_CURRENT_LIST_DIR}/../components/smooth_ui_toolkit/src)
stackpet_radar_add_test(stackpet_radar_servo_test
    ${STACKPET_RADAR_MAIN_DIR}/stackchan/motion/servo.cpp
    ${STACKPET_UITK_DIR}/core/animation/animate/animate.cpp
    ${STACKPET_UITK_DIR}/core/animation/animate_value/animate_value.cpp
    ${STACKPET_UITK_DIR}/core/animation/generators/spring/spring.cpp
    ${STACKPET_UITK_DIR}/core/animation/generators/easing/easing.cpp
    ${STACKPET_UITK_DIR}/core/easing/ease.cpp
    ${STACKPET_UITK_DIR}/core/hal/hal.cpp
)
target_include_directories(stackpet_radar_servo_test BEFORE PRIVATE ${CMAKE_CURRENT_LIST_DIR}/stubs ${STACKPET_UITK_DIR})
target_compile_options(stackpet_radar_servo_test PRIVATE -Wno-unused-parameter -Wno-mismatched-tags)  # their headers
