/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <mooncake.h>
#include <stackpet/camera/frame_grabber.h>
#include <stackpet/fx/head_touch.h>
#include <stackpet/fx/led_fx.h>
#include <stackpet/fx/motion_script.h>
#include <stackpet/fx/screen_tap.h>
#include <stackpet/games/freeze_game.h>
#include <stackpet/games/game_mood.h>
#include <memory>
#include <string>
#include <vector>

/**
 * @brief "一二三木头人" game app (see stackpet/README.md)
 *
 */
class AppFreeze : public mooncake::AppAbility {
public:
    AppFreeze();
    ~AppFreeze();  // defined in the .cpp, where Ui is complete

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    struct Ui;

    void create_ui();
    void handle_event(const stackpet::games::FreezeEvent& event, uint32_t now);
    void enter_phase(stackpet::games::FreezePhase phase, uint32_t now);
    void update_visuals(uint32_t now);
    void update_environment();
    void set_texts(std::string_view title, std::string_view subtitle);
    void set_idle_motion_enabled(bool enabled);
    bool servo_moving(uint32_t now);
    void update_mood(uint32_t now);
    void apply_emotion(stackchan::avatar::Emotion emotion);
    std::string records_text() const;
    std::vector<std::string> idle_lines() const;
    std::string idle_hint() const;
    void load_records();
    void save_records();

    std::unique_ptr<Ui> _ui;
    std::unique_ptr<stackpet::games::FreezeGame> _game;
    stackpet::fx::HeadTouch _head_touch;
    stackpet::fx::ScreenTap _screen_tap;
    stackpet::fx::LedAnimator _leds;
    stackpet::fx::MotionScript _motion_script;
    stackpet::games::MoodScript _mood;
    stackpet::games::WatchMood _watch_mood;
    stackchan::avatar::Emotion _shown_emotion = stackchan::avatar::Emotion::Neutral;
    stackpet::LumaFrame _luma;
    uint32_t _env_checked_seq     = 0;
    bool _camera_ok               = false;
    bool _too_dark                = false;
    bool _low_battery             = false;
    int _idle_motion_modifier_id  = -1;
    int _away_yaw                 = 0;
    float _caught_x               = 0.0f;
    float _caught_y               = 0.0f;
    int _record_best              = 0;  // most rounds survived (NVS)
    int _record_fastest           = 0;  // fewest rounds needed to tag the robot, 0 = never won (NVS)
    uint32_t _ready_ms            = 0;
    int _ready_ticks              = 0;
    uint32_t _next_heartbeat_ms   = 0;
    size_t _idle_line_index       = 0;
    uint32_t _next_idle_line_ms   = 0;
    int _bar_value                = -1;
    uint32_t _bar_color           = 0;
    bool _servo_moving            = false;
    uint32_t _servo_check_ms      = 0;
    std::string _chant_text;
};
