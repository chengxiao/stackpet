/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <mooncake.h>
#include <stackpet/fx/head_touch.h>
#include <stackpet/fx/led_fx.h>
#include <stackpet/fx/screen_tap.h>
#include <cstdint>
#include <memory>

/**
 * @brief "FACE" app: preview the face styles and pick one (see stackpet/face/README.md)
 *
 * The emotions play one after another; tap the left / right third of the screen or swipe the head to switch the
 * style (saved at once, every app uses it), tap the middle to pause, tap the head to hear the face talk.
 */
class AppFace : public mooncake::AppAbility {
public:
    AppFace();
    ~AppFace();  // defined in the .cpp, where Ui is complete

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    struct Ui;

    void create_ui();
    void create_avatar();
    void destroy_avatar();
    void handle_input(uint32_t now);
    void switch_style(int delta, uint32_t now);
    void show_emotion(int index, uint32_t now);
    void show_style_badge(uint32_t now);
    void say_hello(uint32_t now);
    void update_caption();

    std::unique_ptr<Ui> _ui;
    stackpet::fx::HeadTouch _head_touch;
    stackpet::fx::ScreenTap _screen_tap;
    stackpet::fx::LedAnimator _leds;
    int _blink_modifier_id      = -1;
    int _emotion_index          = 0;
    bool _paused                = false;
    uint32_t _next_emotion_ms   = 0;
    uint32_t _badge_hide_ms     = 0;
    uint32_t _swipe_ignore_till = 0;
    uint32_t _speech_clear_ms   = 0;
};
