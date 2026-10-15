# Host (macOS/Linux) renderer for the launcher's screensaver (stackpet/usage/clock_screen.cpp). Usage: ./render.sh
cmake_minimum_required(VERSION 3.16)
project(usage_preview C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(FW ${CMAKE_CURRENT_LIST_DIR}/../..)
set(MAIN ${FW}/main)
# The CN 20 px font made for the host by render.sh (the firmware's comes from the assets partition, a 32-bit layout)
set(CN_FONT "" CACHE FILEPATH "font_preview_cn20.c")

# LVGL: the same v9.4 sources the firmware uses, the face preview's host config
set(LVGL_DIR ${FW}/managed_components/lvgl__lvgl)
file(GLOB_RECURSE LVGL_SRCS ${LVGL_DIR}/src/*.c)
add_library(lvgl STATIC ${LVGL_SRCS})
target_include_directories(lvgl PUBLIC ${LVGL_DIR} ${FW}/tools/face_preview)
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)

add_executable(usage_preview
    usage_preview.cpp
    ui_stub.cpp
    ${MAIN}/stackpet/usage/clock_screen.cpp
    ${MAIN}/stackpet/usage/clock_widgets.cpp
    ${MAIN}/stackpet/usage/style_claude.cpp
    ${MAIN}/stackpet/usage/style_rings.cpp
    ${MAIN}/stackpet/usage/style_terminal.cpp
    ${MAIN}/stackpet/usage/style_sleepy.cpp
    ${MAIN}/stackpet/usage/usage_look.cpp
    ${MAIN}/stackpet/usage/usage_snapshot.cpp
    ${MAIN}/stackpet/usage/fonts/font_usage_clock_96.c
    ${MAIN}/stackpet/usage/fonts/font_usage_clock_52.c
    ${MAIN}/stackpet/usage/fonts/font_usage_num_26.c
    ${MAIN}/stackpet/usage/fonts/font_usage_mono_72.c
    ${MAIN}/stackpet/usage/fonts/font_usage_mono_16.c
    ${CN_FONT}
)
target_include_directories(usage_preview PRIVATE ${MAIN} ${FW}/components/ArduinoJson/src)
target_link_libraries(usage_preview PRIVATE lvgl)
