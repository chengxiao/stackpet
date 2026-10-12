/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <mooncake.h>
#include <stackpet/fx/head_touch.h>
#include <stackpet/fx/led_fx.h>
#include <stackpet/fx/motion_script.h>
#include <stackpet/fx/screen_tap.h>
#include <stackpet/games/spin_game.h>
#include <stackpet/games/game_mood.h>
#include <memory>

/**
 * @brief "幸运转盘" app (see stackpet/README.md)
 *
 */
class AppSpin : public mooncake::AppAbility {
public:
    AppSpin();
    ~AppSpin();  // defined in the .cpp, where Ui is complete

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    struct Ui;

    void create_ui();
    void handle_input(uint32_t now);
    void request_start(uint32_t now);
    void change_category(int delta, uint32_t now);
    void handle_event(const stackpet::games::SpinEvent& event, uint32_t now);
    void enter_phase(stackpet::games::SpinPhase phase, uint32_t now);
    void show_result(int resultIndex, uint32_t now);
    void apply_rotation(uint32_t now);
    void stop_rotation();
    bool servo_moving(uint32_t now);
    const char* idle_hint() const;
    void update_mood(uint32_t now);
    void apply_emotion(stackchan::avatar::Emotion emotion);

    std::unique_ptr<Ui> _ui;
    std::unique_ptr<stackpet::games::SpinGame> _game;
    stackpet::fx::HeadTouch _head_touch;
    stackpet::fx::ScreenTap _screen_tap;
    stackpet::fx::LedAnimator _leds;
    stackpet::fx::MotionScript _motion_script;
    stackpet::games::MoodScript _mood;
    stackchan::avatar::Emotion _shown_emotion = stackchan::avatar::Emotion::Neutral;
    stackpet::games::SpinTone _reveal_tone    = stackpet::games::SpinTone::Pick;
    bool _reveal_decor_pending                = false;
    bool _card_pending                        = false;
    uint32_t _card_show_ms                    = 0;
    bool _start_pending          = false;
    uint32_t _start_pending_ms   = 0;
    bool _rotating               = false;
    int _last_sent_velocity      = 0;
    uint32_t _last_rotate_ms     = 0;
    uint32_t _last_tick_sound_ms = 0;
    bool _servo_moving           = false;
    uint32_t _servo_check_ms     = 0;
    bool _low_battery            = false;
};
