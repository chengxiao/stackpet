# StackPet remote host tests (included from tests/CMakeLists.txt): the ESP-NOW remote's packets, the stick gestures and
# the launcher's stick
set(STACKPET_REMOTE_MAIN_DIR ${CMAKE_CURRENT_LIST_DIR}/../main)

add_executable(stackpet_remote_test
    ${CMAKE_CURRENT_LIST_DIR}/stackpet_remote_test.cpp
    ${STACKPET_REMOTE_MAIN_DIR}/stackpet/remote/remote_packet.cpp
    ${STACKPET_REMOTE_MAIN_DIR}/stackpet/remote/stick_gestures.cpp
    ${STACKPET_REMOTE_MAIN_DIR}/stackpet/remote/stick_navigator.cpp
)
target_include_directories(stackpet_remote_test PRIVATE ${STACKPET_REMOTE_MAIN_DIR} ${CMAKE_CURRENT_LIST_DIR})
target_compile_features(stackpet_remote_test PRIVATE cxx_std_17)
target_compile_options(stackpet_remote_test PRIVATE -Wall -Wextra)
add_test(NAME stackpet_remote_test COMMAND $<TARGET_FILE:stackpet_remote_test>)

# RemoteLink against stand-ins for the HAL, NVS and the log (stubs_remote/ comes first on the include path)
add_executable(stackpet_remote_link_test
    ${CMAKE_CURRENT_LIST_DIR}/stackpet_remote_link_test.cpp
    ${STACKPET_REMOTE_MAIN_DIR}/stackpet/remote/remote_link.cpp
    ${STACKPET_REMOTE_MAIN_DIR}/stackpet/remote/remote_packet.cpp
)
target_include_directories(stackpet_remote_link_test BEFORE PRIVATE ${CMAKE_CURRENT_LIST_DIR}/stubs_remote)
target_include_directories(stackpet_remote_link_test PRIVATE ${STACKPET_REMOTE_MAIN_DIR} ${CMAKE_CURRENT_LIST_DIR})
target_compile_features(stackpet_remote_link_test PRIVATE cxx_std_17)
target_compile_options(stackpet_remote_link_test PRIVATE -Wall -Wextra)
add_test(NAME stackpet_remote_link_test COMMAND $<TARGET_FILE:stackpet_remote_link_test>)
