# StackPet games track host tests (pure C++ modules, no ESP-IDF)
set(STACKPET_MAIN_DIR ${CMAKE_CURRENT_LIST_DIR}/../main)

function(stackpet_games_add_test name)
    add_executable(${name} ${CMAKE_CURRENT_LIST_DIR}/${name}.cpp ${ARGN})
    target_include_directories(${name} PRIVATE ${STACKPET_MAIN_DIR} ${CMAKE_CURRENT_LIST_DIR})
    target_compile_features(${name} PRIVATE cxx_std_17)
    target_compile_options(${name} PRIVATE -Wall -Wextra)
    add_test(NAME ${name} COMMAND $<TARGET_FILE:${name}>)
endfunction()

stackpet_games_add_test(stackpet_games_motion_test
    ${STACKPET_MAIN_DIR}/stackpet/vision/motion_detector.cpp
)

stackpet_games_add_test(stackpet_games_freeze_test
    ${STACKPET_MAIN_DIR}/stackpet/vision/motion_detector.cpp
    ${STACKPET_MAIN_DIR}/stackpet/games/freeze_game.cpp
)

stackpet_games_add_test(stackpet_games_spin_test
    ${STACKPET_MAIN_DIR}/stackpet/games/spin_game.cpp
)

stackpet_games_add_test(stackpet_games_mood_test
    ${STACKPET_MAIN_DIR}/stackpet/games/game_mood.cpp
    ${STACKPET_MAIN_DIR}/stackpet/games/spin_game.cpp
)
