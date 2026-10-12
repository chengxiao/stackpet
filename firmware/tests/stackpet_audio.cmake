# StackPet audio track host tests (included from tests/CMakeLists.txt)
add_executable(stackpet_audio_synth_test
    ${CMAKE_CURRENT_LIST_DIR}/stackpet_audio_synth_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../main/stackpet/sound/tone_synth.cpp
)
target_include_directories(stackpet_audio_synth_test PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../main)
target_compile_features(stackpet_audio_synth_test PRIVATE cxx_std_17)
add_test(NAME stackpet_audio_synth_test COMMAND $<TARGET_FILE:stackpet_audio_synth_test>)

add_executable(stackpet_audio_trim_test
    ${CMAKE_CURRENT_LIST_DIR}/stackpet_audio_trim_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../main/stackpet/sound/pcm_trim.cpp
)
target_include_directories(stackpet_audio_trim_test PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../main)
target_compile_features(stackpet_audio_trim_test PRIVATE cxx_std_17)
add_test(NAME stackpet_audio_trim_test COMMAND $<TARGET_FILE:stackpet_audio_trim_test>)
