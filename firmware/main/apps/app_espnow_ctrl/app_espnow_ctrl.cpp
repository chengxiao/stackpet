/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_espnow_ctrl.h"
#include "view/page_selector.h"
#include "view/view.h"
#include <hal/hal.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <assets/assets.h>
#include <smooth_lvgl.hpp>
#include <stackchan/stackchan.h>
#include <apps/common/common.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <stackpet/face/face.h>
#include <stackpet/remote/remote_actions.h>
#include <stackpet/remote/remote_link.h>
#include <stackpet/remote/stick_gestures.h>
#include <stackpet/sound/sound_player.h>
#include <stackpet/ui/ui.h>

using namespace mooncake;
using namespace smooth_ui_toolkit::lvgl_cpp;
using namespace stackchan;

AppEspnowControl::AppEspnowControl()
{
    // 配置 App 名
    setAppInfo().name = "ESPNOW.REMOTE";
    // 配置 App 图标
    static auto icon  = assets::get_image("icon_controller.bin");
    setAppInfo().icon = (void*)&icon;
    // 配置 App 主题颜色
    static uint32_t theme_color = 0x7ACE74;
    setAppInfo().userData       = (void*)&theme_color;
}

void AppEspnowControl::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

static int _receiver_id  = 0;
static int _wifi_channel = 0;
static bool _is_receiver = false;

// StackPet: the remote's packets come through stackpet::remote::RemoteLink (the launcher listens from boot on), with
// stick gestures on top
static stackpet::remote::StickGestures _gestures;
static stackpet::remote::RemoteActions _actions;
static int _stick_yaw   = 0;
static int _stick_pitch = 450;

void AppEspnowControl::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    // Default setup
    _receiver_id  = 1;
    _wifi_channel = 1;

    auto& link        = stackpet::remote::RemoteLink::instance();
    const bool direct = link.takeDirectReceiver();
    if (direct) {
        // StackPet: opened with the remote (picked on the home screen): receiving right away, on the channel it was
        // heard on, as the receiver id picked last time
        _is_receiver  = true;
        _wifi_channel = link.channel() > 0 ? link.channel() : 1;
        _receiver_id  = link.receiverId();
    } else {
        // Start setup page
        bool is_advanced = start_startup_page();
        if (is_advanced) {
            start_advanced_page();
        }
        if (_is_receiver) {
            link.setReceiverId(_receiver_id);  // StackPet: the launcher and the other apps listen as this one too
        }
    }
    mclog::tagInfo(getAppInfo().name, "get setup: role {}, id {}, channel {}", _is_receiver ? "Receiver" : "Sender",
                   _receiver_id, _wifi_channel);

    // Start espnow (StackPet: through the remote link, which may be listening already)
    link.start(false);
    link.lockChannel(_wifi_channel);
    _gestures.reset();

    // Normal avatar
    LvglLockGuard lock;

    auto& stackchan = GetStackChan();

    auto avatar = stackpet::face::create(lv_screen_active(), stackpet::ui::fontCn20());
    stackchan.attachAvatar(std::move(avatar));
    if (direct) {
        // StackPet: the remote brought us here
        _actions.announce(GetHAL().millis(), "遥控器连上啦～", avatar::Emotion::Happy);
        stackpet::SoundPlayer::instance().playSfx(stackpet::Sfx::Success);
    }

    stackchan.clearModifiers();
    stackchan.addModifier(std::make_unique<BreathModifier>());
    stackchan.addModifier(std::make_unique<BlinkModifier>());

    // Diable auto angle sync to prevent jitter
    stackchan.motion().setAutoAngleSyncEnabled(false);

    GetHAL().setLaserEnabled(false);

    view::create_home_indicator([&]() { close(); }, 0xA0D99C, 0x154311);
    view::create_status_bar(0xA0D99C, 0x154311);
}

bool AppEspnowControl::start_startup_page()
{
    GetHAL().lvglLock();
    auto page = std::make_unique<view::EspnowRoleSelectorPage>();
    GetHAL().lvglUnlock();
    while (1) {
        GetHAL().delay(50);
        LvglLockGuard lock;
        if (page->isSelected()) {
            break;
        }
    }

    bool is_advanced = false;
    GetHAL().lvglLock();
    if (page->selectedIndex() == 0) {
        _is_receiver = true;
    } else if (page->selectedIndex() == 1) {
        _is_receiver = false;
    } else {
        is_advanced = true;
    }
    page.reset();
    GetHAL().lvglUnlock();

    return is_advanced;
}

void AppEspnowControl::start_advanced_page()
{
    // Get role
    std::vector<std::string> role_options = {"Receiver", "Sender"};
    int role_selection                    = view::create_page_selector_and_wait("Select Role", role_options);
    _is_receiver                          = (role_selection == 0);
    mclog::tagInfo(getAppInfo().name, "selected role: {}", _is_receiver ? "Receiver" : "Sender");

    // Get wifi channel
    std::vector<std::string> channel_options;
    for (int i = 0; i < 13; i++) {
        channel_options.push_back(std::to_string(i + 1));
    }
    _wifi_channel = view::create_page_selector_and_wait("Select WiFi Channel", channel_options) + 1;
    mclog::tagInfo(getAppInfo().name, "selected wifi channel: {}", _wifi_channel);

    // Get id
    if (_is_receiver) {
        std::vector<std::string> id_options;
        for (int i = 1; i < 255; i++) {
            id_options.push_back(std::to_string(i));
        }
        _receiver_id = view::create_page_selector_and_wait("Select Receiver ID", id_options) + 1;
        mclog::tagInfo(getAppInfo().name, "selected receiver id: {}", _receiver_id);
    } else {
        std::vector<std::string> id_options;
        for (int i = 0; i < 255; i++) {
            if (i == 0) {
                id_options.push_back("0 (Broadcast)");
                continue;
            }
            id_options.push_back(std::to_string(i));
        }
        _receiver_id = view::create_page_selector_and_wait("Select Receiver ID", id_options);
        mclog::tagInfo(getAppInfo().name, "selected target id: {}", _receiver_id);
    }
}

static void handle_received_data(uint32_t now)
{
    // [target-id (uint8)] [yaw-angle (int16)] [pitch-angle (int16)] [speed (int16)] [laser-enabled (uint8)]
    // ([buttons (uint8)] [channel (uint8)]: StackPet's remote firmware, see stackpet/remote/remote_packet.h)
    // id: 0 for broadcast
    // yaw: -1280 ~ 1280
    // pitch: 0 ~ 900
    // speed: 0 ~ 1000, suggest 600
    // laser-enabled: 0 = off, 1 = on
    const auto packets = stackpet::remote::RemoteLink::instance().update(now);
    for (const auto& packet : packets) {
        _gestures.update(now, packet);
        _stick_yaw   = packet.yaw;
        _stick_pitch = packet.pitch;
        GetHAL().setLaserEnabled(packet.laser);
        if (_actions.busy()) {
            continue;  // the head is a gesture's for a moment, where the stick is when it ends
        }
        auto& motion = GetStackChan().motion();
        motion.moveWithSpeed(packet.yaw, packet.pitch, packet.speed);
    }

    for (const auto& event : _gestures.take()) {
        mclog::info("remote gesture {}", static_cast<int>(event.gesture));
        _actions.start(event, now, _stick_yaw, _stick_pitch);
    }
    _actions.update(now, _stick_yaw, _stick_pitch);
}

void handle_send_pose()
{
    static uint32_t last_send_tick = 0;

    if (GetHAL().millis() - last_send_tick < 50) {
        return;
    }
    last_send_tick = GetHAL().millis();

    // [target-id (uint8)] [yaw-angle (int16)] [pitch-angle (int16)] [speed (int16)] [laser-enabled (uint8)]
    // id: 0 for broadcast
    // yaw: -1280 ~ 1280
    // pitch: 0 ~ 900
    // speed: 0 ~ 1000, suggest 600
    // laser-enabled: 0 = off, 1 = on
    auto& motion        = GetStackChan().motion();
    int16_t yaw_angle   = motion.getCurrentYawAngle();
    int16_t pitch_angle = motion.getCurrentPitchAngle();
    const int16_t speed = 800;

    std::vector<uint8_t> data;
    data.reserve(8);  // 预留空间提高效率

    // [0] target-id
    data.push_back(_receiver_id);

    // [1-2] yaw-angle (小端序：先发低位，再发高位)
    data.push_back(static_cast<uint8_t>(yaw_angle & 0xFF));
    data.push_back(static_cast<uint8_t>((yaw_angle >> 8) & 0xFF));
    // [3-4] pitch-angle
    data.push_back(static_cast<uint8_t>(pitch_angle & 0xFF));
    data.push_back(static_cast<uint8_t>((pitch_angle >> 8) & 0xFF));
    // [5-6] speed
    data.push_back(static_cast<uint8_t>(speed & 0xFF));
    data.push_back(static_cast<uint8_t>((speed >> 8) & 0xFF));

    // [7] laser-enabled
    data.push_back(0);  // always off for sender

    GetHAL().espNowSend(data);
}

void AppEspnowControl::onRunning()
{
    LvglLockGuard lock;

    if (_is_receiver) {
        handle_received_data(GetHAL().millis());
    } else {
        handle_send_pose();
    }

    GetStackChan().update();

    view::update_home_indicator();
    view::update_status_bar();
}

void AppEspnowControl::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    _actions.stop();  // StackPet: a spin gesture under way: the yaw servo back in position mode
    stackpet::SoundPlayer::instance().shutdown();

    LvglLockGuard lock;

    view::destroy_home_indicator();
    view::destroy_status_bar();

    GetHAL().requestWarmReboot(2);
}
