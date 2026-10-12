# Host (macOS/Linux) renderer for the StackPet face styles. Usage: ./render.sh [output dir]
cmake_minimum_required(VERSION 3.16)
project(face_preview C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(FW ${CMAKE_CURRENT_LIST_DIR}/../..)
set(MAIN ${FW}/main)

# LVGL: the same v9.4 sources the firmware uses (managed component), host config in lv_conf.h
set(LVGL_DIR ${FW}/managed_components/lvgl__lvgl)
file(GLOB_RECURSE LVGL_SRCS ${LVGL_DIR}/src/*.c)
add_library(lvgl STATIC ${LVGL_SRCS})
target_include_directories(lvgl PUBLIC ${LVGL_DIR} ${CMAKE_CURRENT_LIST_DIR})
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)

# smooth_ui_toolkit core (animation, math, default chrono HAL); its LVGL C++ wrappers are header only
set(UITK ${FW}/components/smooth_ui_toolkit)
file(GLOB_RECURSE UITK_SRCS ${UITK}/src/core/*.cpp)
add_library(uitk STATIC ${UITK_SRCS})
target_include_directories(uitk PUBLIC ${UITK}/src ${UITK}/src/lvgl)
target_compile_definitions(uitk PUBLIC SMOOTH_UI_TOOLKIT_ENABLE_DEFAULT_HAL=1)
target_link_libraries(uitk PUBLIC lvgl)

# Avatar code exactly as in the firmware (every stackpet/face/*.cpp except the NVS settings)
file(GLOB FACE_SRCS ${MAIN}/stackpet/face/*.cpp ${MAIN}/stackpet/face/*.c ${MAIN}/stackpet/face/assets/*.c)
list(FILTER FACE_SRCS EXCLUDE REGEX ".*face_settings\\.cpp$")
file(GLOB AVATAR_SRCS
    ${MAIN}/stackchan/avatar/skins/default/*.cpp
    ${MAIN}/stackchan/avatar/skins/default/assets/*.c
    ${MAIN}/stackchan/avatar/decorators/*.cpp
    ${MAIN}/stackchan/avatar/decorators/assets/*.c
)
add_executable(face_preview
    face_preview.cpp
    stubs/stackpet_ui_stub.cpp
    ${FACE_SRCS}
    ${AVATAR_SRCS}
    ${MAIN}/stackpet/ui/fonts/font_stackpet_title_28.c
    ${MAIN}/stackpet/ui/fonts/font_stackpet_title_44.c
)
# stubs/ first: <hal/hal.h> must resolve to the host stub, not the firmware HAL
target_include_directories(face_preview PRIVATE ${CMAKE_CURRENT_LIST_DIR}/stubs ${MAIN})
target_link_libraries(face_preview PRIVATE uitk lvgl)
