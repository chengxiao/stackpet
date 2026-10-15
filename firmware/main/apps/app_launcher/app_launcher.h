/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "view/view.h"
#include <apps/app_setup/workers/workers.h>
#include <mooncake.h>
#include <mooncake_templates.h>
#include <stackpet/remote/stick_navigator.h>
#include <stackpet/usage/clock_screen.h>
#include <cstdint>
#include <memory>
#include <vector>

class AppLauncher : public mooncake::templates::AppLauncherBase {
public:
    void onLauncherCreate() override;
    void onLauncherOpen() override;
    void onLauncherRunning() override;
    void onLauncherClose() override;
    void onLauncherDestroy() override;

private:
    std::unique_ptr<view::LauncherView> _view;
    // StackPet: the clock and Claude's usage (stackpet/usage), in place of the bouncing logo
    std::unique_ptr<stackpet::usage::ClockScreen> _screensaver;
    std::unique_ptr<setup_workers::StartupWorker> _startup_worker;
    uint32_t _screensaver_timecount = 0;
    uint8_t _screensaver_brightness = 0;  // StackPet: the backlight before it, back after (lower at night)
    uint8_t _screensaver_backlight  = 0;
    uint32_t _screensaver_power_ms  = 0;     // StackPet: when the cable was looked at last ...
    bool _screensaver_on_cable      = true;  // ... and whether it is in: no dimming then
    bool _startup_checked           = false;
    bool _remote_started            = false;  // StackPet
    bool _app_opened                = false;
    int _remote_control_app         = -1;
    std::vector<int> _listening_apps;
    std::vector<int> _touch_home_apps;  // the remote cannot take them home (BLE, or no WiFi in most of the app)
    stackpet::remote::StickNavigator _navigator;

    void create_launcher_view();
    void screensaver_update();
    void close_screensaver();  // StackPet: and the backlight as it was
    // StackPet: the remote is listened for from boot on (stackpet/remote/remote_link.h), its stick goes through the
    // apps (stackpet/remote/stick_navigator.h)
    void stackpet_remote_start();  // once, from the first tick (or the first touch) on
    void stackpet_remote_update();
    void open_app(int appID, bool keepListening);
    void open_touched(int appID);
    void open_from_remote(int appID);
    bool keeps_listening(int appID) const;
};
