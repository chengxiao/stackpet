/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_remote.h"
#include <hal/hal.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <stackchan/stackchan.h>
#include <stackchan/avatar/decorators/decorators.h>
#include <apps/common/common.h>
#include <assets/assets.h>
#include <stackpet/stackpet.h>
#include <stackpet/camera/frame_grabber.h>
#include <stackpet/fx/led_fx.h>
#include <stackpet/sound/sound_player.h>
#include <stackpet/ui/ui.h>
#include <stackpet/vision/face_bench.h>
#if STACKPET_CAL
#include <stackpet/vision/face_detector.h>
#include <esp_timer.h>
#include <fmt/format.h>
#endif
#include <stackpet/web/web_utils.h>
#include <wifi_manager.h>
#include <wifi_station.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>
#include <stackpet/face/face.h>

using namespace mooncake;
using namespace stackchan;
using namespace uitk::lvgl_cpp;
using stackpet::web::Command;
using stackpet::web::CommandType;
using stackpet::web::WebServer;

namespace {

constexpr uint32_t kThemeColor = 0x3FA9F5;
constexpr uint32_t kThemeDark  = 0x0F2A3D;

// Camera pipeline for the LAN stream: 320x240 @ ~15 fps, no luma (nobody runs vision here)
constexpr uint8_t kJpegQuality = 50;
constexpr uint8_t kMaxFps      = 15;

// The page sends x > 0 for "look right in the camera image". Measured with a face (host tool aim_cal.py, 2026-10-12):
// a positive yaw moves the scene left in the image, i.e. looks right in it (the image is mirrored with respect to the
// head's own left / right), so x maps to +yaw as it is. Same relation as the RADAR's kImageXToYaw.
constexpr float kLookYawSign = 1.0f;

// Continuous rotation for the "spin" action. ~1 turn with the SCS0009 at this PWM value, tune on the device.
constexpr int kSpinVelocity         = 800;
constexpr uint32_t kSpinDurationMs  = 1800;
constexpr uint32_t kSpinWaitMaxMs   = 800;
constexpr uint32_t kSpinReturnMaxMs = 2500;

constexpr uint32_t kNetworkTimeoutMs   = 20000;
constexpr uint32_t kNetworkTaskStack   = 8192;
constexpr uint32_t kStatusPeriodMs     = 500;
constexpr uint32_t kLiveCheckPeriodMs  = 250;
constexpr uint32_t kLiveBlinkMs        = 600;
constexpr uint32_t kLedFrameMs         = 40;  // LED refresh is one I2C transaction to the IO expander
constexpr uint32_t kDecoratorMs        = 3000;
constexpr uint32_t kPhotoCountdownMs   = 3000;
constexpr uint32_t kPhotoLightAtMs     = 2500;  // fill light on early enough for the auto exposure to settle
constexpr uint32_t kPhotoCaptureMaxMs  = 1500;
constexpr uint32_t kFlashMs            = 150;
constexpr uint32_t kCardHintMs         = 5000;
constexpr int kMaxCommandsPerLoop      = 8;
constexpr int kDanceBeatMs             = 430;
constexpr int kYawLimit                = 1280;
constexpr int kPitchMin                = 0;
constexpr int kPitchMax                = 850;
constexpr size_t kTokenLength          = 8;

uint32_t speech_duration_ms(const std::string& text)
{
    // ~150 ms per character (UTF-8 continuation bytes are not counted), 2.5 s minimum, 9 s maximum
    size_t chars = 0;
    for (unsigned char c : text) {
        if ((c & 0xC0) != 0x80) {
            chars++;
        }
    }
    return std::clamp<uint32_t>(2500 + static_cast<uint32_t>(chars) * 150, 2500, 9000);
}

std::string translate_network_message(std::string_view msg)
{
    auto starts_with = [&](std::string_view prefix) { return msg.substr(0, prefix.size()) == prefix; };
    if (starts_with("WiFi scanning")) {
        return "正在扫描 Wi-Fi…";
    }
    if (starts_with("WiFi connecting")) {
        return "正在连接 Wi-Fi…";
    }
    if (starts_with("Connecting to ")) {
        std::string_view ssid = msg.substr(14);
        if (ssid.size() >= 4 && ssid.substr(ssid.size() - 4) == " ...") {
            ssid.remove_suffix(4);
        }
        return fmt::format("正在连接 {}…", ssid);
    }
    if (starts_with("Enter WiFi config mode")) {
        return "还没有配过网：请先在 SETUP 里配网";
    }
    return std::string(msg);
}

// 16 eighth notes on the 430 ms dance beat (185 ms note + 30 ms air), then a long final note on the end pose
std::vector<stackpet::Tone> dance_melody()
{
    static constexpr uint16_t kNotes[16] = {523, 659, 784, 659, 698, 880, 1047, 880,
                                            784, 659, 523, 659, 587, 698, 784, 988};
    std::vector<stackpet::Tone> tones;
    tones.reserve(33);
    for (uint16_t note : kNotes) {
        tones.push_back({note, 185, 45, stackpet::Wave::Square, 0});
        tones.push_back({0, 30, 0, stackpet::Wave::Square, 0});
    }
    tones.push_back({1047, 430, 50, stackpet::Wave::Square, 0});
    return tones;
}

}  // namespace

AppRemote::AppRemote()
{
    // "REMOTE" alone is easily confused with the official ESPNOW remote
    setAppInfo().name = "WEB.REMOTE";
    static auto icon  = assets::get_image("icon_remote.bin");
    setAppInfo().icon = (void*)&icon;
    static uint32_t theme_color = kThemeColor;
    setAppInfo().userData       = (void*)&theme_color;
}

void AppRemote::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

void AppRemote::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");
    _stage             = Stage::Connecting;
    _opened_ms         = GetHAL().millis();
    _visitor_greeted   = false;
    _net_timeout_shown = false;

    LvglLockGuard lock;
    create_loading_ui();
    // Created right away so the user can always swipe up to leave, even if Wi-Fi never connects
    view::create_home_indicator([&]() { close(); }, kThemeColor, kThemeDark);
    view::create_status_bar(kThemeColor, kThemeDark);

    if (!_net_task_started) {
        _net_task_started = true;
        if (xTaskCreate(network_task, "remote_net", kNetworkTaskStack, this, 2, nullptr) != pdPASS) {
            mclog::tagError(getAppInfo().name, "failed to create network task");
            post_network_message("网络任务启动失败");
        }
    }
}

void AppRemote::onRunning()
{
    const uint32_t now = GetHAL().millis();

    LvglLockGuard lock;

    if (_stage == Stage::Connecting) {
        update_connecting(now);
        view::update_home_indicator();
        view::update_status_bar();
        return;
    }

    // Commands from the web page (queued by the HTTP tasks, executed here where LVGL and the servos live)
    Command command;
    for (int i = 0; i < kMaxCommandsPerLoop && WebServer::instance().popCommand(command); i++) {
        handle_command(command, now);
    }

    if (_screen_clicked.exchange(false)) {
        set_card_visible(!_card_visible);
    }

    update_first_visitor(now);
    update_sequence(now);
    update_spin(now);
#if STACKPET_CAL
    update_debug_spin(now);
#endif
    update_dance_leds(now);
    update_photo(now);
    update_live_badge(now);
    update_status(now);

    GetStackChan().update();
    view::update_home_indicator();
    view::update_status_bar();
}

void AppRemote::onClose()
{
    // First of all: a yaw servo left in wheel mode keeps spinning across the warm reboot
    GetStackChan().motion().yawServo().stopRotation();

    mclog::tagInfo(getAppInfo().name, "on close");

    WebServer::instance().stop();
#if STACKPET_CAL
    stackpet::vision::FaceDetector::instance().stop();
#endif
    stackpet::FrameGrabber::instance().stop();
    stackpet::SoundPlayer::instance().shutdown();
    stackpet::fx::off();

    {
        LvglLockGuard lock;
        destroy_widgets();
        GetStackChan().clearModifiers();
        GetStackChan().resetAvatar();
        view::destroy_home_indicator();
        view::destroy_status_bar();
    }

    GetHAL().requestWarmReboot(stackpet::kAppIndexRemote);
}

/* -------------------------------------------------------------------------- */
/*                               Wi-Fi bootstrap                              */
/* -------------------------------------------------------------------------- */

void AppRemote::network_task(void* param)
{
    auto* self = static_cast<AppRemote*>(param);
    // Called exactly once: the HAL network start is not thread-safe, nothing else calls it while this app runs
    GetHAL().startNetwork([self](std::string_view msg) { self->post_network_message(translate_network_message(msg)); });
    self->_net_connected.store(true);
    vTaskDelete(nullptr);
}

void AppRemote::post_network_message(std::string message)
{
    std::lock_guard<std::mutex> lock(_net_mutex);
    _net_message       = std::move(message);
    _net_message_dirty = true;
}

void AppRemote::update_connecting(uint32_t now)
{
    if (_net_connected.load()) {
        enter_ready(now);
        return;
    }

    if (!_net_timeout_shown && now - _opened_ms >= kNetworkTimeoutMs) {
        _net_timeout_shown = true;
        if (_loading_msg) {
            _loading_msg->setText("没连上 Wi-Fi，请先在 SETUP 里配网");
        }
        if (_loading_hint) {
            _loading_hint->setHidden(false);
        }
        return;
    }

    std::string message;
    {
        std::lock_guard<std::mutex> lock(_net_mutex);
        if (!_net_message_dirty) {
            return;
        }
        _net_message_dirty = false;
        message            = _net_message;
    }
    // After the timeout keep the actionable hint on screen, the network task keeps trying in the background
    if (!_net_timeout_shown && _loading_msg) {
        _loading_msg->setText(message);
    }
}

void AppRemote::enter_ready(uint32_t now)
{
    destroy_loading_ui();

    // Modem sleep delays the TCP ACKs by a whole DTIM period and caps the MJPEG stream at a few fps.
    // The warm reboot on exit restores the default power save level.
    WifiManager::GetInstance().SetPowerSaveLevel(WifiPowerSaveLevel::PERFORMANCE);
    _ip = WifiManager::GetInstance().GetIpAddress();

    // A fresh token for every session: only people who scanned the QR code on the screen get in
    std::string token = stackpet::web::utils::makeToken(esp_random, kTokenLength);
    _url              = fmt::format("http://{}/", _ip);
    _qr_url           = fmt::format("http://{}/?k={}", _ip, token);
    WebServer::instance().setAccessToken(token);
#ifdef STACKPET_AUTOOPEN
    // Test builds only (no one can scan the screen during automated tests); release builds never log the token
    mclog::tagWarn(getAppInfo().name, "TEST BUILD remote url: {}", _qr_url);
#endif

    stackpet::GrabberConfig grabber_config;
    grabber_config.jpegQuality = kJpegQuality;
    grabber_config.maxFps      = kMaxFps;
    grabber_config.lumaWidth   = 0;
    grabber_config.lumaHeight  = 0;
    bool camera_ok             = stackpet::FrameGrabber::instance().start(grabber_config);
    bool server_ok             = WebServer::instance().start();
#if STACKPET_FD_BENCH
    // Test builds only: benchmark the face detector on the live camera (the page's snapshot shows what it sees)
    stackpet::vision::startFaceBench();
#endif
#if STACKPET_CAL
    // Test builds only (aim calibration): the RADAR's face detector next to the page, see /debug/faces
    stackpet::vision::FaceDetector::instance().start();
#endif
    mclog::tagInfo(getAppInfo().name, "remote ready at {} (camera {}, server {})", _url, camera_ok, server_ok);

    create_avatar(avatar::Emotion::Neutral);

    create_card(server_ok, camera_ok);
    set_card_visible(true);
    create_live_badge();
    rebuild_system_widgets();

    _opened_ms = now;  // first visitor detection starts now
    _stage     = Stage::Ready;
}

void AppRemote::create_loading_ui()
{
    _loading_panel = std::make_unique<Container>(lv_screen_active());
    _loading_panel->setSize(320, 240);
    _loading_panel->setAlign(LV_ALIGN_CENTER);
    _loading_panel->setRadius(0);
    _loading_panel->setBorderWidth(0);
    _loading_panel->setPaddingAll(0);
    _loading_panel->setBgColor(lv_color_hex(kThemeDark));
    _loading_panel->setBgOpa(LV_OPA_COVER);
    _loading_panel->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    _loading_title = std::make_unique<Label>(_loading_panel->get());
    _loading_title->setTextFont(&lv_font_montserrat_24);
    _loading_title->setTextColor(lv_color_hex(kThemeColor));
    _loading_title->setText("WEB.REMOTE");
    _loading_title->align(LV_ALIGN_CENTER, 0, -42);

    _loading_msg = std::make_unique<Label>(_loading_panel->get());
    _loading_msg->setTextFont(stackpet::ui::fontCn20());
    _loading_msg->setTextColor(lv_color_hex(0xFFFFFF));
    _loading_msg->setLongMode(LV_LABEL_LONG_WRAP);
    _loading_msg->setWidth(280);
    _loading_msg->setTextAlign(LV_TEXT_ALIGN_CENTER);
    _loading_msg->setText("正在连接 Wi-Fi…");
    _loading_msg->align(LV_ALIGN_CENTER, 0, 4);

    _loading_hint = std::make_unique<Label>(_loading_panel->get());
    _loading_hint->setTextFont(stackpet::ui::fontCn20());
    _loading_hint->setTextColor(lv_color_hex(0x8FA1B3));
    _loading_hint->setText("从屏幕底部上滑可以返回");
    _loading_hint->align(LV_ALIGN_CENTER, 0, 56);
    _loading_hint->setHidden(true);
}

void AppRemote::destroy_loading_ui()
{
    _loading_hint.reset();
    _loading_msg.reset();
    _loading_title.reset();
    _loading_panel.reset();
}

void AppRemote::rebuild_system_widgets()
{
    // Keep the status bar and the home indicator above everything created after them
    view::destroy_home_indicator();
    view::destroy_status_bar();
    view::create_home_indicator([&]() { close(); }, kThemeColor, kThemeDark);
    view::create_status_bar(kThemeColor, kThemeDark);
}

/* -------------------------------------------------------------------------- */
/*                                  Commands                                  */
/* -------------------------------------------------------------------------- */

void AppRemote::handle_command(const Command& command, uint32_t now)
{
    auto& stackchan = GetStackChan();
    auto& motion    = stackchan.motion();
    auto& sound     = stackpet::SoundPlayer::instance();

    switch (command.type) {
        case CommandType::Look: {
            if (_spin_state != SpinState::Idle) {
                break;  // The head is busy spinning
            }
            stop_sequence();
            // Keep the spring momentum while the joystick streams targets, resync after a pause
            motion.setAutoAngleSyncEnabled(now - _last_look_ms > 2000);
            _last_look_ms = now;

            float x       = std::clamp(command.x * kLookYawSign, -1.0f, 1.0f);
            float y       = std::clamp(command.y, -1.0f, 1.0f);
            _target_yaw   = static_cast<int>(x * kYawLimit);
            _target_pitch = std::clamp(static_cast<int>((y + 1.0f) * 0.5f * 900.0f), kPitchMin, kPitchMax);
            motion.moveWithSpeed(_target_yaw, _target_pitch, command.a);
            break;
        }
        case CommandType::Home: {
            if (_spin_state != SpinState::Idle) {
                break;
            }
            stop_sequence();
            _target_yaw   = 0;
            _target_pitch = 0;
            motion.setAutoAngleSyncEnabled(true);
            motion.goHome(command.a);
            break;
        }
        case CommandType::Emotion: {
            int emotion = std::clamp(command.a, 0, avatar::kEmotionCount - 1);
            stackchan.avatar().setEmotion(static_cast<avatar::Emotion>(emotion));
            break;
        }
        case CommandType::Decorator:
            add_decorator(command.text);
            break;
        case CommandType::Rgb:
            _rgb[0] = static_cast<uint8_t>(command.a);
            _rgb[1] = static_cast<uint8_t>(command.b);
            _rgb[2] = static_cast<uint8_t>(command.c);
            if (!_dancing && _spin_state == SpinState::Idle && !_photo_light_on) {
                GetHAL().showRgbColor(_rgb[0], _rgb[1], _rgb[2]);
            }
            break;
        case CommandType::Action:
            handle_action(command.text, now);
            break;
        case CommandType::Speak:
            show_speech(command.text, speech_duration_ms(command.text));
            if (!command.text.empty()) {
                stackchan.addModifier(std::make_unique<SpeakingModifier>(2500, 180, false));
                sound.stopAll();
                sound.playSfx(stackpet::Sfx::Boop);
            }
            break;
        case CommandType::Sfx:
            // Tapping a sound button repeatedly must not build up a queue
            sound.stopAll();
            sound.playSfx(static_cast<stackpet::Sfx>(command.a));
            break;
        case CommandType::Volume: {
            // Persisted to NVS by the HAL: done here in the main loop, the page only sends it on release
            uint8_t volume = static_cast<uint8_t>(std::clamp(command.a, 0, 100));
            if (volume != GetHAL().getSpeakerVolume()) {
                GetHAL().setSpeakerVolume(volume, true);
            }
            sound.stopAll();
            sound.playSfx(stackpet::Sfx::Boop);
            break;
        }
        case CommandType::Photo:
            start_photo(static_cast<uint32_t>(command.a), now);
            break;
        case CommandType::Face:
            apply_face_style(command.a);
            break;
#if STACKPET_CAL
        case CommandType::DebugSpin:
            if (_spin_state != SpinState::Idle || _debug_spin_velocity != 0) {
                break;
            }
            stop_sequence();
            motion.setAutoTorqueReleaseEnabled(false);
            motion.setAutoAngleSyncEnabled(true);
            _debug_spin_velocity = command.a;
            _debug_spin_end_ms   = now + static_cast<uint32_t>(command.b);
            _debug_spin_samples.clear();
            mclog::tagInfo(getAppInfo().name, "SPINLOG start v {} for {} ms", command.a, command.b);
            motion.yawServo().rotate(command.a);
            break;
#endif
    }
}

#if STACKPET_CAL
void AppRemote::update_debug_spin(uint32_t now)
{
    if (_debug_spin_velocity == 0) {
        return;
    }
    auto& motion = GetStackChan().motion();
    _debug_spin_samples.emplace_back(static_cast<uint32_t>(esp_timer_get_time() / 1000),
                                     motion.yawServo().readRawPosition());
    if (static_cast<int32_t>(now - _debug_spin_end_ms) < 0) {
        return;
    }
    motion.yawServo().stopRotation();
    motion.setAutoTorqueReleaseEnabled(true);
    // esp_timer ms + raw reading pairs, 16 per line
    std::string line;
    for (size_t i = 0; i < _debug_spin_samples.size(); i++) {
        line += fmt::format(" {}:{}", _debug_spin_samples[i].first, _debug_spin_samples[i].second);
        if ((i + 1) % 16 == 0 || i + 1 == _debug_spin_samples.size()) {
            mclog::tagInfo(getAppInfo().name, "SPINLOG v {}{}", _debug_spin_velocity, line);
            line.clear();
        }
    }
    mclog::tagInfo(getAppInfo().name, "SPINLOG end v {}: {} samples, stopped at raw {}", _debug_spin_velocity,
                   _debug_spin_samples.size(), motion.yawServo().readRawPosition());
    _debug_spin_velocity = 0;
    _debug_spin_samples.clear();
}
#endif

void AppRemote::create_avatar(avatar::Emotion emotion)
{
    uitk::lvgl_cpp::Container* panel = nullptr;
    auto avatar = stackpet::face::create(lv_screen_active(), stackpet::ui::fontCn20(), &panel);
    if (panel) {
        panel->onClick().connect([this]() { _screen_clicked = true; });
        // A rebuilt avatar is the newest object on the screen: push it under the card, the badges and the system
        // widgets again (no-op for the first one)
        panel->moveBackground();
    }
    avatar->setEmotion(emotion);
    GetStackChan().attachAvatar(std::move(avatar));

    // Base modifiers. They keep per-face state (breath offset, head pet decorators), so a new face gets new ones
    GetStackChan().addModifier(std::make_unique<BlinkModifier>());
    GetStackChan().addModifier(std::make_unique<BreathModifier>());
    GetStackChan().addModifier(std::make_unique<HeadPetModifier>());
}

void AppRemote::apply_face_style(int index)
{
    if (index < 0 || index >= static_cast<int>(stackpet::face::Style::Count)) {
        return;  // already validated by the web server
    }
    auto style = static_cast<stackpet::face::Style>(index);
    if (style == stackpet::face::style() && GetStackChan().hasAvatar()) {
        return;
    }
    // NVS write: main loop only
    stackpet::face::setStyle(style);

    // Keep the mood. Decorators, the speech bubble, timed emotions and the head pet state belong to the old face
    // and are transient anyway, so they go with it; the base modifiers are recreated for the new face.
    auto& stackchan = GetStackChan();
    auto emotion    = stackchan.hasAvatar() ? stackchan.avatar().getEmotion() : avatar::Emotion::Neutral;
    stackchan.clearModifiers();
    _speech_modifier_id = -1;
    stackchan.resetAvatar();
    create_avatar(emotion);
    mclog::tagInfo(getAppInfo().name, "face style -> {}", stackpet::face::styleName(style));
}

void AppRemote::handle_action(const std::string& action, uint32_t now)
{
    if (_spin_state != SpinState::Idle) {
        return;
    }

    const int yaw   = _target_yaw;
    const int pitch = _target_pitch;
    auto clamp_yaw  = [](int v) { return std::clamp(v, -kYawLimit, kYawLimit); };

    if (action == "nod") {
        // Pitch 0 is the lowest position, so a nod is "up and back" around the current gaze
        int up = std::clamp(pitch + 260, kPitchMin, kPitchMax);
        start_sequence({{yaw, up, 800, 260}, {yaw, pitch, 800, 260}, {yaw, up, 800, 260}, {yaw, pitch, 600, 400}},
                       now);
    } else if (action == "shake") {
        start_sequence({{clamp_yaw(yaw + 260), pitch, 850, 230},
                        {clamp_yaw(yaw - 260), pitch, 850, 230},
                        {clamp_yaw(yaw + 260), pitch, 850, 230},
                        {clamp_yaw(yaw - 260), pitch, 850, 230},
                        {yaw, pitch, 600, 400}},
                       now);
    } else if (action == "dance") {
        std::vector<MotionStep> steps;
        for (int beat = 0; beat < 8; beat++) {
            int side = (beat % 2 == 0) ? 1 : -1;
            steps.push_back({side * 420, (beat % 4 < 2) ? 260 : 60, 900, kDanceBeatMs});
        }
        steps.push_back({0, 0, 500, 500});
        _emotion_before_dance = static_cast<int>(GetStackChan().avatar().getEmotion());
        GetStackChan().avatar().setEmotion(avatar::Emotion::Happy);
        auto& sound = stackpet::SoundPlayer::instance();
        sound.stopAll();
        sound.playTones(dance_melody());
        start_sequence(std::move(steps), now);
        _dancing      = true;
        _target_yaw   = 0;
        _target_pitch = 0;
    } else if (action == "spin") {
        start_spin(now);
    }
}

void AppRemote::add_decorator(const std::string& name)
{
    auto& avatar     = GetStackChan().avatar();
    lv_obj_t* parent = lv_screen_active();
    if (name == "heart") {
        avatar.addDecorator(std::make_unique<avatar::HeartDecorator>(parent, kDecoratorMs));
    } else if (name == "angry") {
        avatar.addDecorator(std::make_unique<avatar::AngryDecorator>(parent, kDecoratorMs));
    } else if (name == "sweat") {
        avatar.addDecorator(std::make_unique<avatar::SweatDecorator>(parent, kDecoratorMs));
    } else if (name == "shy") {
        avatar.addDecorator(std::make_unique<avatar::ShyDecorator>(parent, kDecoratorMs));
    } else if (name == "dizzy") {
        avatar.addDecorator(std::make_unique<avatar::DizzyDecorator>(parent, kDecoratorMs));
    }
}

void AppRemote::show_speech(const std::string& text, uint32_t durationMs)
{
    auto& stackchan = GetStackChan();
    if (_speech_modifier_id >= 0) {
        // A still running timed speech would clear the new text when it expires. Pool ids are recycled once a
        // modifier destroys itself, so only remove the slot if it still holds a speech modifier (only this app
        // creates them).
        if (dynamic_cast<TimedSpeechModifier*>(stackchan.getModifier(_speech_modifier_id)) != nullptr) {
            stackchan.removeModifier(_speech_modifier_id);
        }
        _speech_modifier_id = -1;
    }
    if (text.empty()) {
        stackchan.avatar().clearSpeech();
        return;
    }
    _speech_modifier_id = stackchan.addModifier(std::make_unique<TimedSpeechModifier>(text, durationMs));
}

/* -------------------------------------------------------------------------- */
/*                              Motion programs                               */
/* -------------------------------------------------------------------------- */

void AppRemote::start_sequence(std::vector<MotionStep> steps, uint32_t now)
{
    stop_sequence();
    if (steps.empty()) {
        return;
    }
    _steps           = std::move(steps);
    _step_index      = 0;
    _step_started_ms = now;
    auto& motion     = GetStackChan().motion();
    motion.setAutoAngleSyncEnabled(false);
    motion.moveWithSpeed(_steps[0].yaw, _steps[0].pitch, _steps[0].speed);
}

void AppRemote::update_sequence(uint32_t now)
{
    if (_steps.empty()) {
        return;
    }
    if (now - _step_started_ms < _steps[_step_index].durationMs) {
        return;
    }
    _step_index++;
    if (_step_index >= _steps.size()) {
        stop_sequence();
        return;
    }
    _step_started_ms = now;
    const auto& step = _steps[_step_index];
    GetStackChan().motion().moveWithSpeed(step.yaw, step.pitch, step.speed);
}

void AppRemote::stop_sequence()
{
    _steps.clear();
    _step_index = 0;
    GetStackChan().motion().setAutoAngleSyncEnabled(true);
    if (_dancing) {
        _dancing = false;
        GetStackChan().avatar().setEmotion(static_cast<avatar::Emotion>(_emotion_before_dance));
        GetHAL().showRgbColor(_rgb[0], _rgb[1], _rgb[2]);
    }
}

void AppRemote::start_spin(uint32_t now)
{
    if (_spin_state != SpinState::Idle) {
        return;
    }
    stop_sequence();
    _spin_velocity = (esp_random() & 1) ? kSpinVelocity : -kSpinVelocity;
    _spin_state    = SpinState::Waiting;
    _spin_state_ms = now;
    _spin_poll_ms  = 0;
    // The spring animation must be finished, otherwise the next position write switches the servo back to
    // position mode; auto torque release would stop the rotation as well
    auto& motion = GetStackChan().motion();
    motion.setAutoTorqueReleaseEnabled(false);
    motion.setAutoAngleSyncEnabled(true);
    auto& sound = stackpet::SoundPlayer::instance();
    sound.stopAll();
    sound.playSfx(stackpet::Sfx::Go);
}

void AppRemote::update_spin(uint32_t now)
{
    auto& motion = GetStackChan().motion();
    switch (_spin_state) {
        case SpinState::Idle:
            return;
        case SpinState::Waiting: {
            // isMoving() polls the servo over UART, keep it at 20 Hz
            if (now - _spin_poll_ms < 50) {
                return;
            }
            _spin_poll_ms = now;
            if (motion.isMoving() && now - _spin_state_ms < kSpinWaitMaxMs) {
                return;
            }
            motion.yawServo().rotate(_spin_velocity);
            _spin_state    = SpinState::Rotating;
            _spin_state_ms = now;
            return;
        }
        case SpinState::Rotating: {
            if (now - _spin_led_ms >= kLedFrameMs) {
                _spin_led_ms = now;
                stackpet::fx::chase((now - _spin_state_ms) / 400.0f, 0x3F, 0xA9, 0xF5);
            }
            if (now - _spin_state_ms < kSpinDurationMs) {
                return;
            }
            // Synchronous: PWM 0, back to position mode, hold where it stopped; then glide home
            motion.yawServo().stopRotation();
            _target_yaw   = 0;
            _target_pitch = 0;
            motion.goHome(600);
            _spin_state    = SpinState::Returning;
            _spin_state_ms = now;
            _spin_poll_ms  = now;
            return;
        }
        case SpinState::Returning: {
            if (now - _spin_poll_ms < 50) {
                return;
            }
            _spin_poll_ms = now;
            if (motion.isMoving() && now - _spin_state_ms < kSpinReturnMaxMs) {
                return;
            }
            motion.setAutoTorqueReleaseEnabled(true);
            GetHAL().showRgbColor(_rgb[0], _rgb[1], _rgb[2]);
            _spin_state = SpinState::Idle;
            return;
        }
    }
}

void AppRemote::update_dance_leds(uint32_t now)
{
    if (!_dancing || now - _dance_led_ms < kLedFrameMs) {
        return;
    }
    _dance_led_ms = now;
    stackpet::fx::rainbow(now / 1500.0f);
}

/* -------------------------------------------------------------------------- */
/*                                    Photo                                   */
/* -------------------------------------------------------------------------- */

void AppRemote::start_photo(uint32_t id, uint32_t now)
{
    if (_photo_state != PhotoState::Idle) {
        mclog::tagWarn(getAppInfo().name, "photo {} ignored, {} still in progress", id, _photo_id);
        return;
    }
    _photo_state       = PhotoState::Countdown;
    _photo_id          = id;
    _photo_started_ms  = now;
    _photo_count_shown = 0;
    _photo_light_on    = false;
    // Make the grabber encode while counting down, so a fresh JPEG is ready the moment the light is on
    stackpet::FrameGrabber::instance().addJpegConsumer();

    _photo_panel = std::make_unique<Container>(lv_screen_active());
    _photo_panel->setSize(110, 110);
    _photo_panel->align(LV_ALIGN_CENTER, 0, 0);
    _photo_panel->setRadius(55);
    _photo_panel->setBorderWidth(0);
    _photo_panel->setBgColor(lv_color_hex(0x000000));
    _photo_panel->setBgOpa(LV_OPA_70);
    _photo_panel->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
    _photo_panel->removeFlag(LV_OBJ_FLAG_CLICKABLE);

    _photo_label = std::make_unique<Label>(_photo_panel->get());
    _photo_label->setTextFont(stackpet::ui::fontTitle44());
    _photo_label->setTextColor(lv_color_hex(0xFFFFFF));
    _photo_label->setText("3");
    _photo_label->align(LV_ALIGN_CENTER, 0, 0);
}

void AppRemote::update_photo(uint32_t now)
{
    if (_flash_panel && now >= _flash_until_ms) {
        _flash_panel.reset();
    }

    auto& grabber = stackpet::FrameGrabber::instance();
    switch (_photo_state) {
        case PhotoState::Idle:
            return;
        case PhotoState::Countdown: {
            uint32_t elapsed = now - _photo_started_ms;
            if (!_photo_light_on && elapsed >= kPhotoLightAtMs) {
                // White fill light; the auto exposure gets ~0.5 s to adapt before the shot
                _photo_light_on = true;
                GetHAL().showRgbColor(255, 255, 255);
            }
            if (elapsed < kPhotoCountdownMs) {
                int count = 3 - static_cast<int>(elapsed / 1000);
                if (count != _photo_count_shown) {
                    _photo_count_shown = count;
                    if (_photo_label) {
                        _photo_label->setText(fmt::format("{}", count));
                    }
                    auto& sound = stackpet::SoundPlayer::instance();
                    sound.stopAll();
                    sound.playSfx(stackpet::Sfx::Tick);
                }
                return;
            }
            // Shoot: the next encoded frame after this point is the photo
            stackpet::JpegFrame latest;
            _photo_seq_mark = grabber.waitJpeg(0, latest, 0) ? latest.seq : 0;
            _photo_label.reset();
            _photo_panel.reset();
            _flash_panel = std::make_unique<Container>(lv_screen_active());
            _flash_panel->setSize(320, 240);
            _flash_panel->setAlign(LV_ALIGN_CENTER);
            _flash_panel->setRadius(0);
            _flash_panel->setBorderWidth(0);
            _flash_panel->setBgColor(lv_color_hex(0xFFFFFF));
            _flash_panel->setBgOpa(LV_OPA_COVER);
            _flash_panel->removeFlag(LV_OBJ_FLAG_CLICKABLE);
            _flash_until_ms = now + kFlashMs;
            auto& sound     = stackpet::SoundPlayer::instance();
            sound.stopAll();
            sound.playSfx(stackpet::Sfx::Click);
            _photo_state      = PhotoState::Capturing;
            _photo_capture_ms = now;
            return;
        }
        case PhotoState::Capturing: {
            stackpet::JpegFrame frame;
            if (grabber.waitJpeg(_photo_seq_mark, frame, 0) && frame.data) {
                WebServer::instance().publishPhoto(_photo_id, frame.data);
                mclog::tagInfo(getAppInfo().name, "photo {} ready ({} bytes)", _photo_id, frame.data->size());
                finish_photo();
            } else if (now - _photo_capture_ms > kPhotoCaptureMaxMs) {
                mclog::tagWarn(getAppInfo().name, "photo {} failed: no frame", _photo_id);
                finish_photo();
            }
            return;
        }
    }
}

void AppRemote::finish_photo()
{
    stackpet::FrameGrabber::instance().removeJpegConsumer();
    _photo_label.reset();
    _photo_panel.reset();
    if (_photo_light_on) {
        _photo_light_on = false;
        GetHAL().showRgbColor(_rgb[0], _rgb[1], _rgb[2]);
    }
    _photo_state = PhotoState::Idle;
}

/* -------------------------------------------------------------------------- */
/*                                Status / card                               */
/* -------------------------------------------------------------------------- */

void AppRemote::update_status(uint32_t now)
{
    if (now - _last_status_ms < kStatusPeriodMs) {
        return;
    }
    _last_status_ms = now;

    stackpet::web::StatusSnapshot status;
    status.batteryLevel = GetHAL().getBatteryLevel();
    status.charging     = GetHAL().isBatteryCharging();
    status.volume       = GetHAL().getSpeakerVolume();
    status.faceStyle    = static_cast<int>(stackpet::face::style());
    status.ip           = _ip;
    if (_spin_state == SpinState::Idle) {
        // Reading the angles talks to the servos over UART; skip it while spinning
        auto angles  = GetStackChan().motion().getCurrentAngles();
        status.yaw   = angles.x;
        status.pitch = angles.y;
    }
    WebServer::instance().setStatus(status);
}

void AppRemote::update_first_visitor(uint32_t now)
{
    if (_visitor_greeted || WebServer::instance().lastRequestMs() <= _opened_ms) {
        return;
    }
    // Somebody opened the page with a valid key: hide the QR card (that also shows how to get it back) and say hi
    _visitor_greeted = true;
    set_card_visible(false);
    auto& avatar = GetStackChan().avatar();
    GetStackChan().addModifier(std::make_unique<TimedEmotionModifier>(avatar::Emotion::Happy, 2000));
    avatar.addDecorator(std::make_unique<avatar::HeartDecorator>(lv_screen_active(), 2000));
    stackpet::SoundPlayer::instance().playSfx(stackpet::Sfx::Success);
    (void)now;
}

void AppRemote::update_live_badge(uint32_t now)
{
    if (!_live_badge) {
        return;
    }
    if (now - _live_checked_ms >= kLiveCheckPeriodMs) {
        _live_checked_ms = now;
        int count        = WebServer::instance().streamClientCount();
        if (count != _live_count) {
            _live_count = count;
            refresh_live_widgets();
        }
    }
    // Blinking dots so it reads as "live" even from the corner of the eye
    if (_live_count > 0 && now - _live_blink_ms >= kLiveBlinkMs) {
        _live_blink_ms = now;
        bool hide      = !_live_dot->hasFlag(LV_OBJ_FLAG_HIDDEN);
        _live_dot->setHidden(hide);
        if (_card_live_dot) {
            _card_live_dot->setHidden(hide);
        }
    }
}

void AppRemote::refresh_live_widgets()
{
    const bool live = _live_count > 0;
    if (_live_badge) {
        // The floating badge sits right on top of the QR code, so it only shows while the card is hidden
        _live_badge->setHidden(!live || _card_visible);
        if (live) {
            _live_label->setText(fmt::format("直播中 · {}人", _live_count));
        }
    }
    if (_card_live_row) {
        _card_live_row->setHidden(!live);
        if (live) {
            _card_live_label->setText(fmt::format("正在直播 · {}人", _live_count));
        }
    }
}

void AppRemote::create_card(bool serverOk, bool cameraOk)
{
    // QR on the left, the instructions on the right (fontCn20: 20 px glyphs, 25 px lines, 8 per line in the
    // column), the full address with the key at the bottom so it can be typed on a computer
    constexpr int kCardWidth  = 312;
    constexpr int kCardHeight = 200;
    constexpr int kPadding    = 10;
    constexpr int kQrSize     = 112;
    constexpr int kTextX      = kPadding + kQrSize + 12;
    constexpr int kTextWidth  = kCardWidth - kTextX - kPadding;
    constexpr int kLineHeight = 25;

    _card = std::make_unique<Container>(lv_screen_active());
    _card->setSize(kCardWidth, kCardHeight);
    _card->align(LV_ALIGN_CENTER, 0, 12);  // top at y = 32, below the 28 px status bar
    _card->setBgColor(lv_color_hex(0xFFFFFF));
    _card->setBgOpa(LV_OPA_COVER);
    _card->setRadius(18);
    _card->setBorderWidth(0);
    _card->setPaddingAll(0);
    _card->setShadowWidth(24);
    _card->setShadowOpa(LV_OPA_40);
    _card->setShadowColor(lv_color_hex(0x000000));
    _card->setScrollbarMode(LV_SCROLLBAR_MODE_OFF);
    _card->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
    _card->onClick().connect([this]() { _screen_clicked = true; });

    if (serverOk) {
        _qrcode = std::make_unique<Qrcode>(_card->get());
        _qrcode->setSize(kQrSize);
        _qrcode->setDarkColor(lv_color_hex(0x0F1720));
        _qrcode->setLightColor(lv_color_hex(0xFFFFFF));
        _qrcode->update(_qr_url);
        _qrcode->align(LV_ALIGN_TOP_LEFT, kPadding, kPadding);
    }

    // Explicit line breaks: automatic wrapping in a 168 px column split words apart
    int y       = kPadding;
    _card_title = std::make_unique<Label>(_card->get());
    _card_title->setTextFont(stackpet::ui::fontCn20());
    _card_title->setTextColor(lv_color_hex(0x0F1720));
    _card_title->setText(serverOk ? "扫码打开遥控台" : "网页服务启动失败");
    _card_title->align(LV_ALIGN_TOP_LEFT, kTextX, y);
    y += kLineHeight + 2;

    _card_hint = std::make_unique<Label>(_card->get());
    _card_hint->setTextFont(stackpet::ui::fontCn20());
    _card_hint->setTextColor(lv_color_hex(0x5B6B7B));
    _card_hint->setText(cameraOk ? "用相机或\n浏览器扫码" : "摄像头不可用\n只能遥控");
    _card_hint->align(LV_ALIGN_TOP_LEFT, kTextX, y);
    y += 2 * kLineHeight + 2;

    _card_warning = std::make_unique<Label>(_card->get());
    _card_warning->setTextFont(stackpet::ui::fontCn20());
    _card_warning->setTextColor(lv_color_hex(0xE8590C));
    _card_warning->setText("扫码的人都能\n看画面、遥控");
    _card_warning->align(LV_ALIGN_TOP_LEFT, kTextX, y);
    y += 2 * kLineHeight + 2;

    // "● 正在直播 · N人": the floating badge is hidden while the card is up, so the card says it itself
    _card_live_row = std::make_unique<Container>(_card->get());
    _card_live_row->setSize(kTextWidth, kLineHeight);
    _card_live_row->align(LV_ALIGN_TOP_LEFT, kTextX, y);
    _card_live_row->setBgOpa(LV_OPA_TRANSP);
    _card_live_row->setBorderWidth(0);
    _card_live_row->setPaddingAll(0);
    _card_live_row->setFlexFlow(LV_FLEX_FLOW_ROW);
    _card_live_row->setFlexAlign(LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    _card_live_row->setPadColumn(6);
    _card_live_row->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
    _card_live_row->removeFlag(LV_OBJ_FLAG_CLICKABLE);

    _card_live_dot = std::make_unique<Container>(_card_live_row->get());
    _card_live_dot->setSize(10, 10);
    _card_live_dot->setRadius(5);
    _card_live_dot->setBorderWidth(0);
    _card_live_dot->setBgColor(lv_color_hex(0xE53935));
    _card_live_dot->setBgOpa(LV_OPA_COVER);
    _card_live_dot->removeFlag(LV_OBJ_FLAG_CLICKABLE);

    _card_live_label = std::make_unique<Label>(_card_live_row->get());
    _card_live_label->setTextFont(stackpet::ui::fontCn20());
    _card_live_label->setTextColor(lv_color_hex(0xE53935));
    _card_live_label->setText("正在直播");
    _card_live_row->setHidden(true);

    // Full address with the key, for typing it by hand (computer browser, or a camera that fails on the QR).
    // Montserrat 16 when it fits, else 14: the longest possible address ("http://255.255.255.255/?k=" + 8)
    // is ~268 px in 14 px, within the 292 px inner width.
    _card_url = std::make_unique<Label>(_card->get());
    _card_url->setTextColor(lv_color_hex(kThemeColor));
    _card_url->setTextFont(&lv_font_montserrat_16);
    _card_url->setText(_qr_url);
    if (_card_url->getWidth() > kCardWidth - 2 * kPadding) {
        _card_url->setTextFont(&lv_font_montserrat_14);
    }
    _card_url->align(LV_ALIGN_BOTTOM_LEFT, kPadding, -kPadding);
}

void AppRemote::create_live_badge()
{
    _live_badge = std::make_unique<Container>(lv_screen_active());
    _live_badge->setSize(LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    _live_badge->align(LV_ALIGN_TOP_LEFT, 8, 34);
    _live_badge->setRadius(14);
    _live_badge->setBorderWidth(0);
    _live_badge->setPadding(3, 3, 10, 12);
    _live_badge->setBgColor(lv_color_hex(0xE53935));
    _live_badge->setBgOpa(LV_OPA_90);
    _live_badge->setFlexFlow(LV_FLEX_FLOW_ROW);
    _live_badge->setFlexAlign(LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    _live_badge->setPadColumn(6);
    _live_badge->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
    _live_badge->removeFlag(LV_OBJ_FLAG_CLICKABLE);

    _live_dot = std::make_unique<Container>(_live_badge->get());
    _live_dot->setSize(10, 10);
    _live_dot->setRadius(5);
    _live_dot->setBorderWidth(0);
    _live_dot->setBgColor(lv_color_hex(0xFFFFFF));
    _live_dot->setBgOpa(LV_OPA_COVER);
    _live_dot->removeFlag(LV_OBJ_FLAG_CLICKABLE);

    _live_label = std::make_unique<Label>(_live_badge->get());
    _live_label->setTextFont(stackpet::ui::fontCn20());
    _live_label->setTextColor(lv_color_hex(0xFFFFFF));
    _live_label->setText("直播中");

    _live_count = 0;
    refresh_live_widgets();
}

void AppRemote::set_card_visible(bool visible)
{
    const bool was_visible = _card_visible;
    _card_visible          = visible;
    if (_card) {
        // Keep the z-order: the status bar and the home indicator (created later) must stay on top
        _card->setHidden(!visible);
    }
    refresh_live_widgets();

    if (was_visible && !visible) {
        // Every time the card goes away (first visitor or a tap): long enough to be noticed by somebody who was
        // looking at the phone at that moment
        show_speech("点屏幕可以再显示二维码", kCardHintMs);
    }
}

void AppRemote::destroy_widgets()
{
    destroy_loading_ui();
    _flash_panel.reset();
    _photo_label.reset();
    _photo_panel.reset();
    _live_label.reset();
    _live_dot.reset();
    _live_badge.reset();
    _card_url.reset();
    _card_live_label.reset();
    _card_live_dot.reset();
    _card_live_row.reset();
    _card_warning.reset();
    _card_hint.reset();
    _card_title.reset();
    _qrcode.reset();
    _card.reset();
    _card_visible = false;
}
