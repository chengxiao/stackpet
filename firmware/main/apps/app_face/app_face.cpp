/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_face.h"
#include <hal/hal.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <stackchan/stackchan.h>
#include <apps/common/common.h>
#include <assets/assets.h>
#include <smooth_lvgl.hpp>
#include <stackpet/stackpet.h>
#include <stackpet/face/face.h>
#include <stackpet/sound/sound_player.h>
#include <stackpet/ui/ui.h>
#include <string>

using namespace mooncake;
using namespace stackchan;
using namespace stackpet;
using namespace uitk::lvgl_cpp;
using stackchan::avatar::Emotion;
using stackpet::face::Style;

static constexpr uint32_t kThemeColor = 0xB28CFF;
static constexpr uint32_t kThemeDark  = 0x2E2447;

static constexpr uint32_t kEmotionMs     = 2000;  // carousel step
static constexpr uint32_t kBadgeMs       = 1500;  // style name after opening / switching
static constexpr uint32_t kSwipeIgnoreMs = 700;   // one head swipe can report several swipes
static constexpr uint32_t kHelloMs       = 1800;
static constexpr int kTapLeftMax         = 107;   // left / right thirds of the 320 px screen switch the style
static constexpr int kTapRightMin        = 213;

struct EmotionEntry {
    Emotion emotion;
    const char* name;
    uint32_t led;      // body LEDs breathe in the colour of the big-eyes style
    const char* line;  // what the face says when its head is tapped
};

static constexpr EmotionEntry kEmotions[] = {
    {Emotion::Neutral, "平静", 0x3CE1FF, "今天也很平静。"},
    {Emotion::Happy, "开心", 0x3CE1FF, "好开心呀！"},
    {Emotion::Laughing, "大笑", 0x3CE1FF, "哈哈哈哈，笑死我了！"},
    {Emotion::Angry, "生气", 0xFF5A32, "哼，我生气了！"},
    {Emotion::Sad, "难过", 0x5A8CFF, "呜呜，有点难过。"},
    {Emotion::Surprised, "惊讶", 0x3CE1FF, "哇，真的吗？"},
    {Emotion::Doubt, "疑惑", 0x3CE1FF, "嗯？这是什么？"},
    {Emotion::Sleepy, "困了", 0x2196AE, "好困，想睡觉了。"},
    {Emotion::Shy, "害羞", 0xFF8CC8, "别一直盯着我看嘛！"},
    {Emotion::Love, "喜欢", 0xFF3C82, "最喜欢你啦！"},
};
static constexpr int kEmotionTotal = sizeof(kEmotions) / sizeof(kEmotions[0]);

struct AppFace::Ui {
    std::unique_ptr<Label> caption;    // emotion name, bottom
    std::unique_ptr<Container> badge;  // style name, bottom, for a moment
    std::unique_ptr<Label> badge_title;
};

AppFace::AppFace()
{
    setAppInfo().name = "FACE";
    static auto icon  = assets::get_image("icon_face.bin");
    setAppInfo().icon = (void*)&icon;
    static uint32_t theme_color = kThemeColor;
    setAppInfo().userData       = (void*)&theme_color;
}

AppFace::~AppFace() = default;

void AppFace::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

void AppFace::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open, style: {}", stackpet::face::styleName(stackpet::face::style()));

    _head_touch.attach();

    LvglLockGuard lock;

    const uint32_t now = GetHAL().millis();
    _emotion_index     = 0;
    _paused            = false;
    create_avatar();
    create_ui();

    view::create_home_indicator([&]() { close(); }, kThemeColor, kThemeDark);
    view::create_status_bar(kThemeColor, kThemeDark);

    show_emotion(0, now);
    show_style_badge(now);
}

void AppFace::create_avatar()
{
    auto& stackchan = GetStackChan();

    uitk::lvgl_cpp::Container* panel = nullptr;
    auto avatar = stackpet::face::create(lv_screen_active(), ui::fontCn20(), &panel);
    if (panel) {
        _screen_tap.attach(panel->get());
        // A rebuilt face is the newest object on the screen: keep it under the captions and the system widgets
        panel->moveBackground();
    }
    avatar->setEmotion(kEmotions[_emotion_index].emotion);
    stackchan.attachAvatar(std::move(avatar));

    // Modifiers keep per-face state (blink base weights, breath offset), so every face gets its own
    _blink_modifier_id = stackchan.addModifier(std::make_unique<BlinkModifier>());
    stackchan.addModifier(std::make_unique<BreathModifier>());
    stackchan.addModifier(std::make_unique<IdleExpressionModifier>(3000, 6000));
}

void AppFace::destroy_avatar()
{
    auto& stackchan = GetStackChan();
    stackchan.clearModifiers();
    _blink_modifier_id = -1;
    stackchan.resetAvatar();
}

void AppFace::create_ui()
{
    _ui = std::make_unique<Ui>();

    _ui->caption = std::make_unique<Label>(lv_screen_active());
    _ui->caption->setTextFont(ui::fontCn20());
    _ui->caption->setTextColor(lv_color_hex(0xC8C8C8));
    _ui->caption->setTextAlign(LV_TEXT_ALIGN_CENTER);
    _ui->caption->setWidth(300);
    _ui->caption->setLongMode(LV_LABEL_LONG_MODE_CLIP);
    _ui->caption->align(LV_ALIGN_BOTTOM_MID, 0, -14);

    _ui->badge = std::make_unique<Container>(lv_screen_active());
    _ui->badge->setSize(250, 62);
    _ui->badge->align(LV_ALIGN_BOTTOM_MID, 0, -8);
    _ui->badge->setRadius(31);
    _ui->badge->setBgColor(lv_color_hex(kThemeDark));
    _ui->badge->setBgOpa(235);
    _ui->badge->setBorderWidth(3);
    _ui->badge->setBorderColor(lv_color_hex(kThemeColor));
    _ui->badge->setPaddingAll(0);
    _ui->badge->setScrollbarMode(LV_SCROLLBAR_MODE_OFF);
    _ui->badge->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
    _ui->badge->removeFlag(LV_OBJ_FLAG_CLICKABLE);
    _ui->badge->setHidden(true);

    _ui->badge_title = std::make_unique<Label>(_ui->badge->get());
    _ui->badge_title->setTextFont(ui::fontTitle44());
    _ui->badge_title->setTextColor(lv_color_hex(0xFFFFFF));
    _ui->badge_title->setTextAlign(LV_TEXT_ALIGN_CENTER);
    _ui->badge_title->setWidth(244);
    _ui->badge_title->setLongMode(LV_LABEL_LONG_MODE_CLIP);
    _ui->badge_title->align(LV_ALIGN_CENTER, 0, 0);
}

void AppFace::onRunning()
{
    const uint32_t now = GetHAL().millis();

    LvglLockGuard lock;

    handle_input(now);

    if (!_paused && static_cast<int32_t>(now - _next_emotion_ms) >= 0) {
        show_emotion((_emotion_index + 1) % kEmotionTotal, now);
    }
    if (_badge_hide_ms != 0 && static_cast<int32_t>(now - _badge_hide_ms) >= 0) {
        _badge_hide_ms = 0;
        _ui->badge->setHidden(true);
        update_caption();
    }
    if (_speech_clear_ms != 0 && static_cast<int32_t>(now - _speech_clear_ms) >= 0) {
        _speech_clear_ms = 0;
        if (GetStackChan().hasAvatar()) {
            GetStackChan().avatar().clearSpeech();
        }
        update_caption();
    }

    _leds.update(now);
    GetStackChan().update();

    view::update_home_indicator();
    view::update_status_bar();
}

void AppFace::handle_input(uint32_t now)
{
    auto head = _head_touch.poll();
    int tap_x = 0;
    int tap_y = 0;
    bool tap  = _screen_tap.poll(tap_x, tap_y);

    if (head.swipe != 0) {
        if (static_cast<int32_t>(now - _swipe_ignore_till) >= 0) {
            _swipe_ignore_till = now + kSwipeIgnoreMs;
            switch_style(head.swipe > 0 ? 1 : -1, now);
        }
        return;
    }

    if (tap) {
        if (tap_x < kTapLeftMax) {
            switch_style(-1, now);
        } else if (tap_x > kTapRightMin) {
            switch_style(1, now);
        } else {
            _paused = !_paused;
            if (!_paused) {
                _next_emotion_ms = now + kEmotionMs;
            }
            SoundPlayer::instance().playSfx(Sfx::Click);
            update_caption();
        }
        return;
    }

    if (head.tapped) {
        say_hello(now);
    }
}

void AppFace::switch_style(int delta, uint32_t now)
{
    const int count = static_cast<int>(Style::Count);
    const int next  = (static_cast<int>(stackpet::face::style()) + delta + count) % count;
    stackpet::face::setStyle(static_cast<Style>(next));  // NVS write, main loop only
    mclog::tagInfo(getAppInfo().name, "style -> {}", stackpet::face::styleName(static_cast<Style>(next)));

    // The speech bubble and the modifiers belong to the old face
    _speech_clear_ms = 0;
    destroy_avatar();
    create_avatar();

    show_style_badge(now);
    SoundPlayer::instance().playSfx(Sfx::Boop);
    _leds.flash(kThemeColor, 300, now);
    // A fresh look at the current emotion before the carousel moves on
    _next_emotion_ms = now + kEmotionMs;
}

void AppFace::show_emotion(int index, uint32_t now)
{
    _emotion_index   = index;
    _next_emotion_ms = now + kEmotionMs;

    auto& stackchan = GetStackChan();
    if (stackchan.hasAvatar()) {
        stackchan.avatar().setEmotion(kEmotions[index].emotion);
        // The official skin moves the eyelids with the emotion: let the blink restore the new opening
        auto blink = static_cast<BlinkModifier*>(stackchan.getModifier(_blink_modifier_id));
        if (blink) {
            blink->resyncEyeWeights();
        }
    }
    _leds.set(fx::LedMode::Pulse, kEmotions[index].led, 2000);
    update_caption();
}

void AppFace::show_style_badge(uint32_t now)
{
    _ui->badge_title->setText(std::string("‹ ") + stackpet::face::styleName(stackpet::face::style()) + " ›");
    _ui->badge->setHidden(false);
    _badge_hide_ms = now + kBadgeMs;
    update_caption();
}

void AppFace::say_hello(uint32_t now)
{
    auto& stackchan = GetStackChan();
    if (!stackchan.hasAvatar()) {
        return;
    }
    stackchan.avatar().setSpeech(kEmotions[_emotion_index].line);
    stackchan.addModifier(std::make_unique<SpeakingModifier>(kHelloMs, 180, false));
    _speech_clear_ms = now + kHelloMs + 400;
    // Let the sentence finish before the carousel moves on
    if (static_cast<int32_t>(_next_emotion_ms - _speech_clear_ms) < 0) {
        _next_emotion_ms = _speech_clear_ms;
    }
    update_caption();
}

void AppFace::update_caption()
{
    if (!_ui) {
        return;
    }
    // The badge and the speech bubble sit where the caption is
    const bool covered = _badge_hide_ms != 0 || _speech_clear_ms != 0;
    _ui->caption->setHidden(covered);
    std::string text = kEmotions[_emotion_index].name;
    if (_paused) {
        text += " · 已暂停";
    }
    _ui->caption->setText(text);
}

void AppFace::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    _head_touch.detach();
    SoundPlayer::instance().shutdown();

    {
        LvglLockGuard lock;
        _ui.reset();
        destroy_avatar();
        view::destroy_home_indicator();
        view::destroy_status_bar();
    }

    GetHAL().showRgbColor(0, 0, 0);
    GetHAL().requestWarmReboot(stackpet::kAppIndexFace);
}
