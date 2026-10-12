/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_freeze.h"
#include <hal/hal.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <stackchan/stackchan.h>
#include <apps/common/common.h>
#include <assets/assets.h>
#include <assets/lang_config.h>
#include <smooth_lvgl.hpp>
#include <settings.h>
#include <stackpet/stackpet.h>
#include <stackpet/sound/sound_player.h>
#include <stackpet/sound/tone_synth.h>
#include <stackpet/ui/ui.h>
#include <stackpet/vision/motion_detector.h>
#include <esp_random.h>
#include <fmt/format.h>
#include <algorithm>
#include <cmath>
#include <stackpet/face/face.h>

using namespace mooncake;
using namespace stackchan;
using namespace stackpet;
using namespace stackpet::games;
using namespace uitk::lvgl_cpp;

static constexpr uint32_t kThemeColor = 0xFF5A5F;
static constexpr uint32_t kThemeDark  = 0x40161A;

/* ------------------------------ Calibration ------------------------------- */
// Values in 0.1 degree. Tune them on the device.
static constexpr int kFacePitch       = 150;    // pitch that looks at a player standing in front
static constexpr int kAwayYaw         = 1100;   // turn-away angle while chanting (random side every round)
static constexpr int kYawLeftSign     = 1;      // +yaw turns the head to the robot's left (lookAtPoint convention)
static constexpr bool kCameraMirrorX  = false;  // set true if the robot looks to the wrong side when it catches you
static constexpr int kCaughtYawSpan   = 330;    // yaw towards a mover at the image edge (~ half camera FOV)
static constexpr int kCaughtPitchSpan = 220;
static constexpr float kDarkLuma      = 28.0f;  // average luma below this: too dark to play fair
static constexpr float kBrightLuma    = 36.0f;  // hysteresis: back above this clears the warning
static constexpr int kLowBattery      = 15;     // percent

/* ---------------------------------- Colors --------------------------------- */
static constexpr uint32_t kLedGo    = 0x00C853;  // "green light": chanting, move now
static constexpr uint32_t kLedStop  = 0xFF1744;  // "red light": watching
static constexpr uint32_t kLedReady = 0x2979FF;
static constexpr uint32_t kLedWhite = 0xFFFFFF;

static constexpr uint32_t kCountdownMs = 3000;  // last part of Ready: one tick per second, LEDs go out one by one
static constexpr uint32_t kIdleLineMs  = 4000;  // Idle subtitle rotates between the rules and the records

static constexpr const char* kRecordsNs = "stackpet_frz";

static const char* kChantWords[] = {"一", "二", "三", "木头人！"};

struct AppFreeze::Ui {
    std::unique_ptr<Label> title;
    std::unique_ptr<Label> subtitle;
    std::unique_ptr<Bar> danger_bar;
};

// Time from the start of the "木头人" melody to its last note "人": the robot snaps back on that note
static uint32_t melody_last_note_offset_ms()
{
    const auto& tones = synth::sfxTones(Sfx::MuTouRen);
    uint32_t offset   = 0;
    for (size_t i = 0; i + 1 < tones.size(); i++) {
        offset += tones[i].durationMs;
    }
    return offset;
}

AppFreeze::AppFreeze()
{
    setAppInfo().name = "FREEZE";
    static auto icon  = assets::get_image("icon_freeze.bin");
    setAppInfo().icon = (void*)&icon;
    static uint32_t theme_color = kThemeColor;
    setAppInfo().userData       = (void*)&theme_color;
}

AppFreeze::~AppFreeze() = default;

void AppFreeze::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

void AppFreeze::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    FreezeConfig config;
    config.seed                 = esp_random() | 1;
    config.finalWordTurnDelayMs = melody_last_note_offset_ms();
    _ready_ms                   = config.readyMs;
    _game                       = std::make_unique<FreezeGame>(config);

    load_records();

    // Vision only: luma frames, no JPEG encoding
    GrabberConfig grabber_config;
    grabber_config.maxFps     = 15;
    grabber_config.lumaWidth  = 40;
    grabber_config.lumaHeight = 30;
    _camera_ok                = FrameGrabber::instance().start(grabber_config);
    if (!_camera_ok) {
        mclog::tagWarn(getAppInfo().name, "camera unavailable, motion detection disabled");
    }

    _head_touch.attach();

    LvglLockGuard lock;

    uitk::lvgl_cpp::Container* avatar_panel = nullptr;
    auto avatar = stackpet::face::create(lv_screen_active(), ui::fontCn20(), &avatar_panel);
    _screen_tap.attach(avatar_panel->get());
    GetStackChan().attachAvatar(std::move(avatar));
    _mood          = MoodScript{};
    _shown_emotion = avatar::Emotion::Neutral;  // a fresh avatar starts neutral

    GetStackChan().addModifier(std::make_unique<BlinkModifier>());
    GetStackChan().addModifier(std::make_unique<BreathModifier>(0, 6));

    create_ui();

    view::create_home_indicator([&]() { close(); }, kThemeColor, kThemeDark);
    view::create_status_bar(kThemeColor, kThemeDark);

    enter_phase(FreezePhase::Idle, GetHAL().millis());
}

void AppFreeze::create_ui()
{
    _ui = std::make_unique<Ui>();

    _ui->title = std::make_unique<Label>(lv_screen_active());
    _ui->title->setTextFont(ui::fontTitle44());
    _ui->title->setTextColor(lv_color_hex(0xFFFFFF));
    _ui->title->setTextAlign(LV_TEXT_ALIGN_CENTER);
    _ui->title->setWidth(316);
    _ui->title->setLongMode(LV_LABEL_LONG_MODE_CLIP);
    _ui->title->align(LV_ALIGN_TOP_MID, 0, 4);

    _ui->subtitle = std::make_unique<Label>(lv_screen_active());
    _ui->subtitle->setTextFont(ui::fontTitle28());
    _ui->subtitle->setTextColor(lv_color_hex(kThemeColor));
    _ui->subtitle->setTextAlign(LV_TEXT_ALIGN_CENTER);
    _ui->subtitle->setWidth(316);
    _ui->subtitle->setLongMode(LV_LABEL_LONG_MODE_CLIP);
    _ui->subtitle->align(LV_ALIGN_TOP_MID, 0, 52);

    // How close the player is to being caught (full = caught). Big enough to read from the start line.
    _ui->danger_bar = std::make_unique<Bar>(lv_screen_active());
    _ui->danger_bar->setSize(260, 30);
    _ui->danger_bar->align(LV_ALIGN_BOTTOM_MID, 0, -22);
    _ui->danger_bar->setRange(0, 100);
    _ui->danger_bar->setValue(0);
    _ui->danger_bar->setRadius(15);
    _ui->danger_bar->setRadius(15, LV_PART_INDICATOR);
    _ui->danger_bar->setBgColor(lv_color_hex(0x333333));
    _ui->danger_bar->setBgOpa(LV_OPA_COVER);
    _ui->danger_bar->setBgColor(lv_color_hex(kLedGo), LV_PART_INDICATOR);
    _ui->danger_bar->removeFlag(LV_OBJ_FLAG_CLICKABLE);
    _ui->danger_bar->setHidden(true);
}

void AppFreeze::onRunning()
{
    const uint32_t now = GetHAL().millis();

    auto head    = _head_touch.poll();
    int tap_x    = 0;
    int tap_y    = 0;
    bool tapped  = _screen_tap.poll(tap_x, tap_y);
    bool started = false;

    if (_camera_ok) {
        FrameGrabber::instance().latestLuma(_luma.seq, _luma);
    }
    const bool audio_busy = SoundPlayer::instance().isBusy();

    LvglLockGuard lock;

    auto& motion = GetStackChan().motion();

    update_environment();

    if (_game->phase() == FreezePhase::Idle && (head.pressed || tapped)) {
        SoundPlayer::instance().playSfx(Sfx::Click);
        _game->start(now);
        started = true;
    } else if (_game->phase() == FreezePhase::CaughtWait) {
        // Party elimination: tap = the others play on, head = new game
        if (tapped) {
            SoundPlayer::instance().playSfx(Sfx::Click);
            _game->continueGame(now);
        } else if (head.pressed) {
            SoundPlayer::instance().playSfx(Sfx::Click);
            _game->start(now);
            started = true;
        }
    }

    FreezeInput input;
    input.nowMs       = now;
    input.headTouched = head.pressed && !started;
    input.servoMoving = servo_moving(now);
    input.audioBusy   = audio_busy;
    if (_camera_ok && !_luma.pixels.empty()) {
        input.luma       = _luma.pixels.data();
        input.lumaWidth  = _luma.width;
        input.lumaHeight = _luma.height;
        input.lumaSeq    = _luma.seq;
    }
    _game->update(input);

    for (const auto& event : _game->takeEvents()) {
        handle_event(event, now);
    }

    update_mood(now);
    update_visuals(now);
    _motion_script.update(now, motion);

    GetStackChan().update();

    view::update_home_indicator();
    view::update_status_bar();
}

void AppFreeze::update_environment()
{
    // Only while the robot faces the players, to warn before a game that would not be fair
    auto phase = _game->phase();
    if (!_camera_ok || _luma.pixels.empty() || _luma.seq == _env_checked_seq ||
        !(phase == FreezePhase::Idle || phase == FreezePhase::Ready)) {
        return;
    }
    _env_checked_seq = _luma.seq;

    float mean    = vision::meanLuma(_luma.pixels.data(), _luma.width, _luma.height);
    bool too_dark = _too_dark ? mean < kBrightLuma : mean < kDarkLuma;
    if (too_dark == _too_dark) {
        return;
    }
    _too_dark = too_dark;

    if (phase == FreezePhase::Idle) {
        GetStackChan().avatar().setSpeech(idle_hint());
    } else {
        _ui->subtitle->setText(_too_dark ? "太暗啦，开灯再玩" : "摸到我的头就算你赢");
    }
}

void AppFreeze::handle_event(const FreezeEvent& event, uint32_t now)
{
    switch (event.type) {
        case FreezeEventType::PhaseEntered:
            enter_phase(event.phase, now);
            break;

        case FreezeEventType::ChantWord: {
            int index   = std::clamp(event.index, 0, 3);
            auto& sound = SoundPlayer::instance();
            switch (index) {
                case 0: sound.playOgg(Lang::Sounds::OGG_1); break;
                case 1: sound.playOgg(Lang::Sounds::OGG_2); break;
                case 2: sound.playOgg(Lang::Sounds::OGG_3); break;
                default: sound.playSfx(Sfx::MuTouRen); break;
            }

            if (index < 3) {
                _chant_text += _chant_text.empty() ? "" : "、";
                _chant_text += kChantWords[index];
                _leds.flash(kLedWhite, 160, now);
            } else {
                _chant_text = kChantWords[3];
                _leds.flash(kLedStop, 500, now);
            }
            _ui->title->setText(_chant_text);
            break;
        }

        case FreezeEventType::Caught:
            _caught_x = event.x;
            _caught_y = event.y;
            break;

        case FreezeEventType::RoundSurvived:
            break;
    }
}

void AppFreeze::enter_phase(FreezePhase phase, uint32_t now)
{
    auto& stackchan = GetStackChan();
    auto& avatar    = stackchan.avatar();
    auto& motion    = stackchan.motion();
    auto& sound     = SoundPlayer::instance();

    avatar.clearDecorators();
    _ui->danger_bar->setHidden(phase != FreezePhase::Watching || !_camera_ok);
    _motion_script.stop();
    // The face acts out the phase (see game_mood.h); decorators follow the beats in update_mood()
    _mood.play(freezeMood(phase), now);

    switch (phase) {
        case FreezePhase::Idle: {
            _low_battery = !GetHAL().isBatteryCharging() && GetHAL().getBatteryLevel() < kLowBattery;
            _idle_line_index   = 0;
            _next_idle_line_ms = now + kIdleLineMs;
            set_texts("一二三木头人", idle_lines().front());
            avatar.setSpeech(idle_hint());
            _leds.set(fx::LedMode::Pulse, kThemeColor, 2400);
            motion.moveWithSpeed(0, kFacePitch, 300);
            set_idle_motion_enabled(true);
            break;
        }

        case FreezePhase::Ready:
            set_idle_motion_enabled(false);
            avatar.clearSpeech();
            set_texts("快退到起点！", _too_dark ? "太暗啦，开灯再玩" : "摸到我的头就算你赢");
            _leds.set(fx::LedMode::Pulse, kLedReady, 800);
            _ready_ticks = 0;
            motion.moveWithSpeed(0, kFacePitch, 500);
            sound.playSfx(Sfx::Start);
            break;

        case FreezePhase::Chanting:
            // Laughing with the eyes squeezed shut: "I'm not peeking"
            avatar.clearSpeech();
            _chant_text.clear();
            set_texts("", "快来摸我的头！");
            _leds.set(fx::LedMode::Pulse, kLedGo, 1000);
            _away_yaw = ((esp_random() & 1) ? 1 : -1) * kAwayYaw;
            motion.moveWithSpeed(_away_yaw, kFacePitch, 600);
            break;

        case FreezePhase::TurningBack:
            set_texts("木头人！", "不许动！");
            // Static from here to the end of Watching: changing LED light would look like motion to the camera
            _leds.set(fx::LedMode::Solid, kLedStop);
            motion.moveWithSpeed(0, kFacePitch, 1000);
            break;

        case FreezePhase::Settling:
            break;

        case FreezePhase::Watching:
            set_texts("不许动！", fmt::format("第 {} 轮", _game->round() + 1));
            _ui->danger_bar->setValue(0);
            _bar_value         = 0;
            _next_heartbeat_ms = now;
            _watch_mood.reset(now);
            break;

        case FreezePhase::Survived:
            set_texts("很好～", fmt::format("第 {} 轮通过！", _game->round()));
            _leds.set(fx::LedMode::Solid, kLedGo);
            sound.playSfx(Sfx::Success);
            // Little nod of approval
            _motion_script.play({{0, 0, kFacePitch + 120, 600}, {250, 0, kFacePitch, 600}}, now);
            break;

        case FreezePhase::Caught: {
            // Cut whatever is still playing, the verdict must be instant
            sound.stopAll();

            int rounds = _game->round();
            // A game that used "continue" does not count for the record: caught -> continue would farm it
            bool beats_record = rounds > _record_best;
            bool record       = beats_record && !_game->hasContinued();
            if (beats_record && !record) {
                avatar.setSpeech("本局有人出局，不计纪录");
            }
            if (rounds == 0) {
                set_texts("抓到你了！", "一上来就被抓啦，再来！");
            } else {
                set_texts("抓到你了！", record ? fmt::format("坚持了 {} 轮，新纪录！", rounds)
                                              : fmt::format("坚持了 {} 轮", rounds));
            }
            if (record) {
                _record_best = rounds;
                save_records();
            }

            _leds.set(fx::LedMode::Pulse, kLedStop, 500);
            _leds.flash(kLedStop, 900, now);
            sound.playSfx(Sfx::Caught);
            sound.playSfx(Sfx::Fail);

            // Look at whoever moved most
            float image_x = kCameraMirrorX ? _caught_x : -_caught_x;
            int yaw       = static_cast<int>(kYawLeftSign * image_x * kCaughtYawSpan);
            int pitch     = std::clamp(static_cast<int>(kFacePitch - _caught_y * kCaughtPitchSpan), 50, 850);
            motion.moveWithSpeed(yaw, pitch, 800);
            break;
        }

        case FreezePhase::PlayerWin: {
            sound.stopAll();

            int round_number = _game->round() + 1;  // the round in which the player tagged the robot
            set_texts("你赢啦！", fmt::format("第 {} 轮摸到我啦！", round_number));
            bool fastest = _record_fastest == 0 || round_number < _record_fastest;
            bool best    = !_game->hasContinued() && _game->round() > _record_best;
            if (fastest || best) {
                _record_fastest = fastest ? round_number : _record_fastest;
                _record_best    = best ? _game->round() : _record_best;
                save_records();
            }
            if (fastest) {
                avatar.setSpeech("最快获胜新纪录！");
            }

            _leds.set(fx::LedMode::Rainbow, 0, 1200);
            sound.playSfx(Sfx::Win);
            // Happy wiggle facing the winner
            _motion_script.play({{0, 0, kFacePitch, 800},
                                 {300, 150, kFacePitch + 80, 700},
                                 {600, -150, kFacePitch + 80, 700},
                                 {900, 150, kFacePitch + 80, 700},
                                 {1200, 0, kFacePitch, 500}},
                                now);
            break;
        }

        case FreezePhase::CaughtWait:
            set_texts("抓到你了！", "点屏幕继续 · 摸头重开");
            avatar.setSpeech("被抓的人点屏幕，其他人别动");
            _leds.set(fx::LedMode::Pulse, kThemeColor, 1600);
            motion.moveWithSpeed(0, kFacePitch, 300);
            break;
    }
}

void AppFreeze::update_visuals(uint32_t now)
{
    auto phase = _game->phase();

    if (phase == FreezePhase::Idle && static_cast<int32_t>(now - _next_idle_line_ms) >= 0) {
        // New players see the rules, regulars see the records
        auto lines         = idle_lines();
        _idle_line_index   = (_idle_line_index + 1) % lines.size();
        _next_idle_line_ms = now + kIdleLineMs;
        _ui->subtitle->setText(lines[_idle_line_index]);
    }

    if (phase == FreezePhase::Ready) {
        // Countdown in the last seconds: one tick per second, the 12 LEDs go out one by one
        uint32_t elapsed   = now - _game->phaseStartMs();
        uint32_t remaining = elapsed < _ready_ms ? _ready_ms - elapsed : 0;
        if (remaining <= kCountdownMs) {
            _leds.set(fx::LedMode::Gauge, kLedReady);
            _leds.setGaugeLevel(static_cast<float>(remaining) / kCountdownMs);
            int due = remaining <= 1000 ? 3 : (remaining <= 2000 ? 2 : 1);
            while (_ready_ticks < due) {
                _ready_ticks++;
                SoundPlayer::instance().playSfx(Sfx::Tick);
            }
        }
    }

    if (phase == FreezePhase::Watching && _camera_ok) {
        // Danger: 100% means the motion is at the "caught" threshold
        float level = _game->motionLevel() / std::max(_game->currentThreshold(), 1e-3f);
        int value   = std::clamp(static_cast<int>(level * 100.0f), 0, 100);
        if (value != _bar_value) {
            _bar_value = value;
            _ui->danger_bar->setValue(value, LV_ANIM_ON);
        }
        uint32_t color = value < 50 ? kLedGo : (value < 80 ? 0xFFC400 : kLedStop);
        if (color != _bar_color) {
            _bar_color = color;
            _ui->danger_bar->setBgColor(lv_color_hex(color), LV_PART_INDICATOR);
        }
        // Somebody almost moved too much: the eyes widen
        apply_emotion(_watch_mood.update(level, now));
    }

    if (phase == FreezePhase::Watching) {
        // Tense heartbeat
        if (static_cast<int32_t>(now - _next_heartbeat_ms) >= 0) {
            _next_heartbeat_ms = now + 650;
            if (!SoundPlayer::instance().isBusy()) {
                // "dum-dum": low square tones (the small speaker cannot play real bass)
                SoundPlayer::instance().playTones(
                    {{210, 50, 35, Wave::Square}, {0, 80}, {180, 60, 30, Wave::Square}});
            }
        }
    }

    _leds.update(now);
}

void AppFreeze::update_mood(uint32_t now)
{
    avatar::Emotion emotion;
    if (!_mood.update(now, emotion)) {
        return;
    }
    apply_emotion(emotion);

    // Decorators follow the beats of the scripts in freezeMood()
    auto& avatar = GetStackChan().avatar();
    auto phase   = _game->phase();
    if (phase == FreezePhase::Caught) {
        if (emotion == avatar::Emotion::Angry) {
            avatar.addDecorator(std::make_unique<avatar::AngryDecorator>(lv_screen_active(), 0, 500));
        } else if (emotion == avatar::Emotion::Laughing) {
            avatar.clearDecorators();  // the anger is over, now it is just mocking
        }
    } else if (phase == FreezePhase::PlayerWin && emotion == avatar::Emotion::Love) {
        avatar.addDecorator(std::make_unique<avatar::HeartDecorator>(lv_screen_active(), 0, 500));
        avatar.addDecorator(std::make_unique<avatar::ShyDecorator>(lv_screen_active(), 0));
    } else if (phase == FreezePhase::Survived && emotion == avatar::Emotion::Love) {
        avatar.addDecorator(std::make_unique<avatar::HeartDecorator>(lv_screen_active(), 1400, 500));
    }
}

void AppFreeze::apply_emotion(avatar::Emotion emotion)
{
    // Every setEmotion() makes the skins blink, so only call it on a real change
    if (emotion == _shown_emotion) {
        return;
    }
    _shown_emotion = emotion;
    GetStackChan().avatar().setEmotion(emotion);
}

void AppFreeze::set_texts(std::string_view title, std::string_view subtitle)
{
    _ui->title->setText(title);
    _ui->subtitle->setText(subtitle);
}

void AppFreeze::set_idle_motion_enabled(bool enabled)
{
    auto& stackchan = GetStackChan();
    if (enabled && _idle_motion_modifier_id < 0) {
        _idle_motion_modifier_id = stackchan.addModifier(std::make_unique<IdleMotionModifier>());
    } else if (!enabled && _idle_motion_modifier_id >= 0) {
        stackchan.removeModifier(_idle_motion_modifier_id);
        _idle_motion_modifier_id = -1;
    }
}

std::string AppFreeze::records_text() const
{
    // No spaces around the numbers: two-digit records must still fit the 316 px label
    if (_record_best > 0 && _record_fastest > 0) {
        return fmt::format("最高{}轮·最快{}轮获胜", _record_best, _record_fastest);
    }
    if (_record_best > 0) {
        return fmt::format("最高纪录{}轮", _record_best);
    }
    if (_record_fastest > 0) {
        return fmt::format("最快{}轮获胜", _record_fastest);
    }
    return "";
}

std::vector<std::string> AppFreeze::idle_lines() const
{
    std::vector<std::string> lines = {"数数时靠近，回头时别动", "背景别有人走动哦"};
    auto records                   = records_text();
    if (!records.empty()) {
        lines.push_back(records);
    }
    return lines;
}

std::string AppFreeze::idle_hint() const
{
    if (!_camera_ok) {
        return "摄像头没开：只比谁先摸到我";
    }
    if (_too_dark) {
        return "太暗啦，开灯再玩";
    }
    if (_low_battery) {
        return "电量不多了，记得充电～";
    }
    return "摸摸我的头就开始";
}

void AppFreeze::load_records()
{
    Settings settings(kRecordsNs, false);
    _record_best    = std::max<int>(0, settings.GetInt("best", 0));
    _record_fastest = std::max<int>(0, settings.GetInt("fastest", 0));
}

void AppFreeze::save_records()
{
    // Main loop only; commits when `settings` goes out of scope
    Settings settings(kRecordsNs, true);
    settings.SetInt("best", _record_best);
    settings.SetInt("fastest", _record_fastest);
}

bool AppFreeze::servo_moving(uint32_t now)
{
    // isMoving() reads the servo over UART once the animation is done, so poll it at 20 Hz only
    if (_servo_check_ms == 0 || now - _servo_check_ms >= 50) {
        _servo_check_ms = now == 0 ? 1 : now;
        _servo_moving   = GetStackChan().motion().isMoving();
    }
    return _servo_moving;
}

void AppFreeze::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    _head_touch.detach();
    SoundPlayer::instance().shutdown();
    FrameGrabber::instance().stop();

    {
        LvglLockGuard lock;

        GetStackChan().clearModifiers();
        _idle_motion_modifier_id = -1;
        _ui.reset();
        GetStackChan().resetAvatar();

        view::destroy_home_indicator();
        view::destroy_status_bar();
    }

    GetHAL().showRgbColor(0, 0, 0);
    GetHAL().requestWarmReboot(stackpet::kAppIndexFreeze);
}
