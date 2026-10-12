/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_spin.h"
#include <hal/hal.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <stackchan/stackchan.h>
#include <apps/common/common.h>
#include <assets/assets.h>
#include <smooth_lvgl.hpp>
#include <stackpet/stackpet.h>
#include <stackpet/sound/sound_player.h>
#include <stackpet/ui/ui.h>
#include <esp_random.h>
#include <algorithm>
#include <cstdlib>
#include <string>
#include <stackpet/face/face.h>

using namespace mooncake;
using namespace stackchan;
using namespace stackpet;
using namespace stackpet::games;
using namespace uitk::lvgl_cpp;

static constexpr uint32_t kThemeColor = 0xFFC53D;
static constexpr uint32_t kThemeDark  = 0x403110;

/* ------------------------------ Calibration ------------------------------- */
static constexpr int kFacePitch          = 150;   // 0.1 degree, looking at people around the table
static constexpr int kSettleSpeed        = 450;
static constexpr uint32_t kStartWaitMs   = 1500;  // max wait for a running head move before spinning
static constexpr int kVelocityResendStep = 20;
static constexpr uint32_t kRotateRefresh = 300;
static constexpr uint32_t kTickSoundGap  = 60;
static constexpr size_t kBigTextMaxChars = 6;     // longer results use the smaller wrapped font
static constexpr int kLowBattery         = 15;    // percent
static constexpr const char* kAgainHint  = "摸头再转 · 点两边换类别";
static constexpr uint32_t kCardDelayMs   = 1200;  // the face reacts first ("ta-da" + reaction), then the card covers it

struct AppSpin::Ui {
    std::unique_ptr<Label> title;
    std::unique_ptr<Container> card;
    std::unique_ptr<Label> card_top;
    std::unique_ptr<Label> card_result;
    std::unique_ptr<Label> card_hint;
};

static size_t utf8_length(const char* text)
{
    size_t count = 0;
    for (const char* p = text; *p; p++) {
        if ((static_cast<unsigned char>(*p) & 0xC0) != 0x80) {
            count++;
        }
    }
    return count;
}

static bool is_spinning(SpinPhase phase)
{
    return phase == SpinPhase::SpinUp || phase == SpinPhase::Cruise || phase == SpinPhase::SlowDown;
}

AppSpin::AppSpin()
{
    setAppInfo().name = "SPIN";
    static auto icon  = assets::get_image("icon_spin.bin");
    setAppInfo().icon = (void*)&icon;
    static uint32_t theme_color = kThemeColor;
    setAppInfo().userData       = (void*)&theme_color;
}

AppSpin::~AppSpin() = default;

void AppSpin::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

void AppSpin::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    SpinConfig config;
    config.seed        = esp_random() | 1;
    config.maxVelocity = 800;
    config.yawLimit    = 1250;  // reach the people sitting beside / behind the robot too
    config.revealMs    = 0;     // keep the result until somebody starts the next spin
    _game              = std::make_unique<SpinGame>(config);

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

    enter_phase(SpinPhase::Idle, GetHAL().millis());
}

void AppSpin::create_ui()
{
    _ui = std::make_unique<Ui>();

    _ui->title = std::make_unique<Label>(lv_screen_active());
    _ui->title->setTextFont(ui::fontTitle44());
    _ui->title->setTextColor(lv_color_hex(0xFFFFFF));
    _ui->title->setTextAlign(LV_TEXT_ALIGN_CENTER);
    _ui->title->setWidth(316);
    _ui->title->setLongMode(LV_LABEL_LONG_MODE_CLIP);
    _ui->title->align(LV_ALIGN_TOP_MID, 0, 4);

    // Result card, drawn over the face
    _ui->card = std::make_unique<Container>(lv_screen_active());
    _ui->card->setSize(300, 170);
    _ui->card->align(LV_ALIGN_CENTER, 0, 14);
    _ui->card->setRadius(18);
    _ui->card->setBgColor(lv_color_hex(kThemeDark));
    _ui->card->setBgOpa(240);
    _ui->card->setBorderWidth(3);
    _ui->card->setBorderColor(lv_color_hex(kThemeColor));
    _ui->card->setPaddingAll(8);
    _ui->card->setScrollbarMode(LV_SCROLLBAR_MODE_OFF);
    _ui->card->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
    _ui->card->removeFlag(LV_OBJ_FLAG_CLICKABLE);
    _ui->card->setHidden(true);

    _ui->card_top = std::make_unique<Label>(_ui->card->get());
    _ui->card_top->setTextFont(ui::fontTitle28());
    _ui->card_top->setTextColor(lv_color_hex(kThemeColor));
    _ui->card_top->setTextAlign(LV_TEXT_ALIGN_CENTER);
    _ui->card_top->setWidth(280);
    _ui->card_top->setLongMode(LV_LABEL_LONG_MODE_CLIP);
    _ui->card_top->align(LV_ALIGN_TOP_MID, 0, 0);

    _ui->card_result = std::make_unique<Label>(_ui->card->get());
    _ui->card_result->setTextColor(lv_color_hex(0xFFFFFF));
    _ui->card_result->setTextAlign(LV_TEXT_ALIGN_CENTER);
    _ui->card_result->setWidth(280);
    _ui->card_result->setLongMode(LV_LABEL_LONG_MODE_WRAP);
    _ui->card_result->align(LV_ALIGN_CENTER, 0, 2);

    _ui->card_hint = std::make_unique<Label>(_ui->card->get());
    _ui->card_hint->setTextFont(ui::fontCn20());
    _ui->card_hint->setTextColor(lv_color_hex(0xBFBFBF));
    _ui->card_hint->setTextAlign(LV_TEXT_ALIGN_CENTER);
    _ui->card_hint->setWidth(280);
    _ui->card_hint->setLongMode(LV_LABEL_LONG_MODE_CLIP);
    _ui->card_hint->align(LV_ALIGN_BOTTOM_MID, 0, 0);
    _ui->card_hint->setText(kAgainHint);
}

void AppSpin::onRunning()
{
    const uint32_t now = GetHAL().millis();

    LvglLockGuard lock;

    auto& motion = GetStackChan().motion();

    handle_input(now);

    // A head move (e.g. the reveal nod) must finish first, its position updates would cancel PWM rotation
    if (_start_pending && (!servo_moving(now) || now - _start_pending_ms > kStartWaitMs)) {
        _start_pending = false;
        _motion_script.stop();
        _game->start(now);
    }

    _game->update(now, servo_moving(now));
    for (const auto& event : _game->takeEvents()) {
        handle_event(event, now);
    }

    update_mood(now);
    if (_card_pending && static_cast<int32_t>(now - _card_show_ms) >= 0) {
        _card_pending = false;
        _ui->card->setHidden(false);
    }

    apply_rotation(now);

    _leds.setChasePosition(_game->wheelPosition());
    _leds.update(now);
    _motion_script.update(now, motion);

    GetStackChan().update();

    view::update_home_indicator();
    view::update_status_bar();
}

void AppSpin::handle_input(uint32_t now)
{
    auto head = _head_touch.poll();
    int tap_x = 0;
    int tap_y = 0;
    bool tap  = _screen_tap.poll(tap_x, tap_y);

    auto phase = _game->phase();
    if (_start_pending || !(phase == SpinPhase::Idle || phase == SpinPhase::Reveal)) {
        return;
    }

    if (head.swipe != 0) {
        change_category(head.swipe > 0 ? 1 : -1, now);
        return;
    }

    if (tap) {
        if (tap_x < 107) {
            change_category(-1, now);
            return;
        }
        if (tap_x > 213) {
            change_category(1, now);
            return;
        }
        request_start(now);
        return;
    }

    if (head.tapped) {
        request_start(now);
    }
}

void AppSpin::request_start(uint32_t now)
{
    SoundPlayer::instance().playSfx(Sfx::Click);
    _start_pending    = true;
    _start_pending_ms = now;
}

void AppSpin::change_category(int delta, uint32_t now)
{
    if (_game->phase() == SpinPhase::Reveal) {
        _game->reset();
    }
    if (delta > 0) {
        _game->nextCategory();
    } else {
        _game->prevCategory();
    }
    SoundPlayer::instance().playSfx(Sfx::Boop);
    _leds.flash(kThemeColor, 200, now);
    enter_phase(SpinPhase::Idle, now);
}

void AppSpin::handle_event(const SpinEvent& event, uint32_t now)
{
    switch (event.type) {
        case SpinEventType::PhaseEntered:
            enter_phase(event.phase, now);
            break;

        case SpinEventType::Tick: {
            auto& sound = SoundPlayer::instance();
            if (now - _last_tick_sound_ms >= kTickSoundGap && !sound.isBusy()) {
                _last_tick_sound_ms = now;
                sound.playSfx(Sfx::Tick);
            }
            break;
        }

        case SpinEventType::Reveal:
            show_result(event.resultIndex, now);
            break;
    }
}

void AppSpin::enter_phase(SpinPhase phase, uint32_t now)
{
    auto& stackchan = GetStackChan();
    auto& avatar    = stackchan.avatar();
    auto& motion    = stackchan.motion();

    // The face acts out the phase (see game_mood.h); an empty script keeps the current one
    _mood.play(spinMood(phase), now);
    if (phase != SpinPhase::Reveal) {
        _card_pending         = false;
        _reveal_decor_pending = false;
    }

    switch (phase) {
        case SpinPhase::Idle:
            stop_rotation();
            _low_battery = !GetHAL().isBatteryCharging() && GetHAL().getBatteryLevel() < kLowBattery;
            avatar.clearDecorators();
            _ui->card->setHidden(true);
            // The arrows match the left / right tap zones that switch the category
            _ui->title->setText(std::string("‹ ") + _game->currentCategory().name + " ›");
            avatar.setSpeech(idle_hint());
            _leds.set(fx::LedMode::Rainbow, 0, 4000);
            motion.setAutoTorqueReleaseEnabled(true);
            motion.moveWithSpeed(0, kFacePitch, 300);
            break;

        case SpinPhase::SpinUp:
            avatar.clearDecorators();
            avatar.clearSpeech();
            avatar.addDecorator(std::make_unique<avatar::DizzyDecorator>(lv_screen_active(), 0, 300));
            _ui->card->setHidden(true);
            _ui->title->setText(_game->currentCategory().name);
            _leds.set(fx::LedMode::Chase, kThemeColor);
            SoundPlayer::instance().playSfx(Sfx::Go);

            // Continuous rotation: keep the torque on and stop the auto release while in PWM mode
            motion.setAutoTorqueReleaseEnabled(false);
            motion.yawServo().setTorqueEnabled(true);
            _rotating           = true;
            _last_sent_velocity = 0;
            _last_rotate_ms     = 0;
            break;

        case SpinPhase::Cruise:
            break;

        case SpinPhase::SlowDown:
            break;

        case SpinPhase::Settle:
            // Stop the wheel right where it is, then glide to the chosen direction in position mode
            stop_rotation();
            avatar.clearDecorators();
            _leds.set(fx::LedMode::Pulse, kThemeColor, 500);
            motion.moveWithSpeed(_game->targetYaw(), kFacePitch, kSettleSpeed);
            break;

        case SpinPhase::Reveal:
            motion.setAutoTorqueReleaseEnabled(true);
            break;
    }
}

void AppSpin::show_result(int resultIndex, uint32_t now)
{
    auto& avatar        = GetStackChan().avatar();
    const auto& category = _game->currentCategory();

    // "Ta-da" (wide eyes), then a reaction that fits the result; its decorator comes with the reaction beat
    _reveal_tone          = spinResultTone(_game->category(), resultIndex);
    _reveal_decor_pending = true;
    _mood.play(spinRevealMood(_reveal_tone), now);
    SoundPlayer::instance().playSfx(Sfx::Reveal);
    _leds.set(fx::LedMode::Solid, kThemeColor);
    _leds.flash(kThemeColor, 700, now);

    if (category.items.empty() || resultIndex < 0) {
        // Pick-a-person: the screen now faces the chosen one
        _ui->title->setText("就是你！");
        _ui->card->setHidden(true);
        avatar.setSpeech(kAgainHint);
    } else {
        const char* result = category.items[resultIndex];
        std::string top;
        if (category.pointAtPerson) {
            top = std::string(category.name) + " · 就是你！";
        } else if (std::string_view(category.name) == "吃什么") {
            top = "今天就吃";
        } else {
            top = "答案是";
        }
        _ui->title->setText("");
        _ui->card_top->setText(top);
        _ui->card_result->setTextFont(utf8_length(result) <= kBigTextMaxChars ? ui::fontTitle44()
                                                                               : ui::fontTitle28());
        _ui->card_result->setText(result);
        // Shown after the face reacted (the card covers the eyes)
        _card_pending = true;
        _card_show_ms = now + kCardDelayMs;
        avatar.clearSpeech();
    }

    // Small nod towards the result
    int yaw = _game->targetYaw();
    _motion_script.play({{150, yaw, kFacePitch + 100, 600}, {450, yaw, kFacePitch, 600}}, now);
}

void AppSpin::update_mood(uint32_t now)
{
    avatar::Emotion emotion;
    if (!_mood.update(now, emotion)) {
        return;
    }
    apply_emotion(emotion);

    // The reveal reaction brings its decorator along (after the "ta-da" beat)
    if (_reveal_decor_pending && _game->phase() == SpinPhase::Reveal && emotion != avatar::Emotion::Surprised) {
        _reveal_decor_pending = false;
        auto& avatar          = GetStackChan().avatar();
        switch (_reveal_tone) {
            case SpinTone::Food:
            case SpinTone::Yes:
                avatar.addDecorator(std::make_unique<avatar::HeartDecorator>(lv_screen_active(), 0, 500));
                break;
            case SpinTone::Truth:
                avatar.addDecorator(std::make_unique<avatar::ShyDecorator>(lv_screen_active(), 0));
                break;
            case SpinTone::Dare:
            case SpinTone::No:
                avatar.addDecorator(std::make_unique<avatar::SweatDecorator>(lv_screen_active(), 0, 700));
                break;
            case SpinTone::Pick:
            case SpinTone::Joke:
                break;
        }
    }
}

void AppSpin::apply_emotion(avatar::Emotion emotion)
{
    // Every setEmotion() makes the skins blink, so only call it on a real change
    if (emotion == _shown_emotion) {
        return;
    }
    _shown_emotion = emotion;
    GetStackChan().avatar().setEmotion(emotion);
}

void AppSpin::apply_rotation(uint32_t now)
{
    if (!_rotating) {
        return;
    }
    if (!is_spinning(_game->phase())) {
        return;
    }

    int velocity = _game->velocity();
    if (std::abs(velocity - _last_sent_velocity) >= kVelocityResendStep || now - _last_rotate_ms >= kRotateRefresh) {
        GetStackChan().motion().yawServo().rotate(velocity);
        _last_sent_velocity = velocity;
        _last_rotate_ms     = now;
    }
}

void AppSpin::stop_rotation()
{
    if (!_rotating) {
        return;
    }
    _rotating = false;
    // PWM 0, back to position mode, hold where the head is
    GetStackChan().motion().yawServo().stopRotation();
}

const char* AppSpin::idle_hint() const
{
    if (_low_battery) {
        return "电量不多了，记得充电～";
    }
    if (_game->currentCategory().pointAtPerson) {
        // The picks cover about +-125 degrees, only right behind the robot is out of reach
        return "别坐我背后哦，摸头开始转！";
    }
    return "摸摸头，开始转！";
}

bool AppSpin::servo_moving(uint32_t now)
{
    // isMoving() reads the servo over UART once the animation is done, so poll it at 20 Hz only
    if (_servo_check_ms == 0 || now - _servo_check_ms >= 50) {
        _servo_check_ms = now == 0 ? 1 : now;
        _servo_moving   = GetStackChan().motion().isMoving();
    }
    return _servo_moving;
}

void AppSpin::onClose()
{
    // First of all: never leave the servo spinning in PWM mode (SoundPlayer::shutdown() may wait up to 500 ms)
    GetStackChan().motion().yawServo().stopRotation();
    _rotating = false;

    mclog::tagInfo(getAppInfo().name, "on close");

    _head_touch.detach();
    SoundPlayer::instance().shutdown();

    {
        LvglLockGuard lock;

        GetStackChan().motion().setAutoTorqueReleaseEnabled(true);

        GetStackChan().clearModifiers();
        _ui.reset();
        GetStackChan().resetAvatar();

        view::destroy_home_indicator();
        view::destroy_status_bar();
    }

    GetHAL().showRgbColor(0, 0, 0);
    GetHAL().requestWarmReboot(stackpet::kAppIndexSpin);
}
