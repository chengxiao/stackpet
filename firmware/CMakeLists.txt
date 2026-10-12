# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "1.5.1")
add_definitions(-DFIRMWARE_VERSION=\"${PROJECT_VER}\")

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

# Auto-load a local sdkconfig overlay (git-ignored) when present. Lets
# self-hosted / custom deployments pin CONFIG_STACKCHAN_SERVER_URL,
# CONFIG_OTA_URL, etc. without touching committed defaults.
# Manual equivalent: SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.local" idf.py build
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults.local")
    set(SDKCONFIG_DEFAULTS
        "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults;${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults.local"
        CACHE STRING "ESP-IDF sdkconfig default files (auto-extended with local overlay)" FORCE)
    message(STATUS "StackChan: detected sdkconfig.defaults.local, applying overlay")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(stack-chan)

# StackPet: esp-sr's prebuilt libdl_lib.a (an old esp-dl) defines kernels with the same names as esp-dl 3.x
# (dl_tie728_s8_conv2d_33cn, ...) but with a different argument layout, and the linker binds esp-dl's calls to them:
# the RADAR face detector then computes garbage ("faces" on a black image). Right before every link, give esp-dl's
# symbols that any prebuilt archive of a managed component also defines a private name (the prebuilt libraries keep
# theirs), see tools/espdl_unclash.py. Found by pattern, not by name, so a renamed component cannot skip the check.
idf_build_get_property(stackpet_components BUILD_COMPONENTS)
set(stackpet_espdl "")
foreach(stackpet_component ${stackpet_components})
    if(stackpet_component MATCHES "(^|__)esp[-_]dl$")
        set(stackpet_espdl ${stackpet_component})
    endif()
endforeach()
if(stackpet_espdl)
    idf_build_get_property(stackpet_python PYTHON)
    idf_component_get_property(stackpet_espdl_lib ${stackpet_espdl} COMPONENT_LIB)
    idf_component_get_property(stackpet_espdl_dir ${stackpet_espdl} COMPONENT_DIR)
    file(GLOB_RECURSE stackpet_prebuilt_libs "${CMAKE_CURRENT_SOURCE_DIR}/managed_components/*.a")
    list(FILTER stackpet_prebuilt_libs INCLUDE REGEX "/${IDF_TARGET}/")
    list(FILTER stackpet_prebuilt_libs EXCLUDE REGEX "^${stackpet_espdl_dir}/")
    list(LENGTH stackpet_prebuilt_libs stackpet_prebuilt_count)
    if(stackpet_prebuilt_count EQUAL 0)
        message(FATAL_ERROR "StackPet: no prebuilt ${IDF_TARGET} archives found to check esp-dl against")
    endif()
    message(STATUS "StackPet: esp-dl symbols are checked against ${stackpet_prebuilt_count} prebuilt archives")
    add_custom_command(TARGET ${CMAKE_PROJECT_NAME}.elf PRE_LINK
        COMMAND ${stackpet_python} ${CMAKE_CURRENT_SOURCE_DIR}/tools/espdl_unclash.py
                --nm ${CMAKE_NM} --objcopy ${CMAKE_OBJCOPY} --check-dir ${CMAKE_BINARY_DIR}/esp-idf
                --lib $<TARGET_FILE:${stackpet_espdl_lib}> ${stackpet_prebuilt_libs}
        VERBATIM)
endif()
