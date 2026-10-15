/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_launcher.h"
#include <hal/hal.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <stackchan/stackchan.h>
#include <stackpet/remote/remote_link.h>
#include <stackpet/usage/usage_feed.h>
#include <algorithm>
#include <cstdint>

using namespace mooncake;

void AppLauncher::onLauncherCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");

    // 打开自己
    open();
}

void AppLauncher::onLauncherOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    LvglLockGuard lock;

    if (!_startup_checked && !GetHAL().isAppConfiged()) {
        mclog::tagInfo(getAppInfo().name, "app not configured, start startup worker");
        _startup_worker = std::make_unique<setup_workers::StartupWorker>();
    } else {
        create_launcher_view();
    }
}

void AppLauncher::onLauncherRunning()
{
    LvglLockGuard lock;

    if (_startup_worker) {
        _startup_worker->update();
        if (_startup_worker->isDone()) {
            _startup_worker.reset();
            _startup_checked = true;
            create_launcher_view();
        }
    } else {
        _view->update();
        screensaver_update();
        stackpet_remote_update();

#ifdef STACKPET_AUTOOPEN
        // StackPet test hook (idf.py -DSTACKPET_AUTOOPEN=<launcher index> build): open that app once after boot
        static bool autoopen_done = false;
        if (!autoopen_done && !_app_opened) {
            autoopen_done = true;
            auto props    = getAppProps();
            if (STACKPET_AUTOOPEN >= 0 && STACKPET_AUTOOPEN < static_cast<int>(props.size())) {
                mclog::tagInfo(getAppInfo().name, "autoopen app index: {}", STACKPET_AUTOOPEN);
#ifdef STACKPET_AUTOOPEN_REMOTE
                // As if the remote opened it: on the saved channel, listening on where the remote's apps do
                stackpet_remote_start();
                open_from_remote(props[STACKPET_AUTOOPEN].appID);
#else
                open_app(props[STACKPET_AUTOOPEN].appID, false);
#endif
            }
        }
#endif
    }

    GetStackChan().update();
}

void AppLauncher::onLauncherClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    LvglLockGuard lock;

    _view.reset();
}

void AppLauncher::onLauncherDestroy()
{
    mclog::tagInfo(getAppInfo().name, "on close");
}

void AppLauncher::create_launcher_view()
{
    _view = std::make_unique<view::LauncherView>();
    _view->init(getAppProps());
    _view->onAppClicked = [&](int appID) {
        mclog::tagInfo(getAppInfo().name, "handle open app, app id: {}", appID);
        open_touched(appID);
    };
}

void AppLauncher::screensaver_update()
{
    const uint32_t SCREENSAVER_TIMEOUT_MS = 30000;

    auto& feed         = stackpet::usage::UsageFeed::instance();
    const uint32_t now = GetHAL().millis();
    uint32_t idle_time = lv_display_get_inactive_time(NULL);
    if (idle_time >= SCREENSAVER_TIMEOUT_MS) {
        if (!_screensaver) {
            // StackPet: the clock and Claude's usage in the style picked last, drawn at once; the remote's stick
            // starts afresh on it (left and right change the style there)
            _screensaver            = std::make_unique<stackpet::usage::ClockScreen>(feed.style());
            _screensaver_brightness = GetHAL().getBackLightBrightness();
            _screensaver_backlight  = _screensaver_brightness;
            _screensaver_timecount  = now - 1000;
            _screensaver_power_ms   = now - 2000;
            _navigator.reset(now);
        }
    } else if (_screensaver) {
        close_screensaver();
    }

    // StackPet: the usage fetched from the Mac while the screensaver shows; the USB console sets where the Mac is
    feed.pollConsole(now);
    feed.update(now, _screensaver != nullptr);

    // A few times a second: the clock turns over on time, the rest only redraws what changed
    if (_screensaver && now - _screensaver_timecount > 250) {
        _screensaver_timecount = now;
        if (_screensaver->style() != feed.style()) {
            _screensaver->setStyle(feed.style());  // picked over USB meanwhile
        }
        stackpet::usage::ClockModel model;
        feed.fill(model, now);
        const bool night = _screensaver->show(model);
        if (now - _screensaver_power_ms >= 2000) {
            _screensaver_power_ms   = now;
            _screensaver_on_cable   = GetHAL().hasExternalPower();  // the PMIC over I2C: not every redraw
        }
        const uint8_t backlight =
            stackpet::usage::screensaverBrightness(_screensaver_brightness, night, _screensaver_on_cable);
        if (backlight != _screensaver_backlight) {
            GetHAL().setBackLightBrightness(backlight, false);
            _screensaver_backlight = backlight;
        }
    }
}

void AppLauncher::close_screensaver()
{
    _screensaver.reset();
    if (_screensaver_backlight != _screensaver_brightness) {
        GetHAL().setBackLightBrightness(_screensaver_brightness, false);
        _screensaver_backlight = _screensaver_brightness;
    }
}

/* -------------------------------------------------------------------------- */
/*                         StackPet: the remote from boot on                  */
/* -------------------------------------------------------------------------- */
void AppLauncher::stackpet_remote_start()
{
    if (_remote_started) {
        return;
    }
    _remote_started = true;
    // Which apps keep listening, which the remote cannot take home (by name), and the remote control app (by name, it
    // gets no role page)
    for (const auto& props : getAppProps()) {
        if (stackpet::remote::appKeepsListening(props.info.name)) {
            _listening_apps.push_back(props.appID);
        } else if (!stackpet::remote::appListensOnItsWifi(props.info.name)) {
            _touch_home_apps.push_back(props.appID);
        }
        if (props.info.name == "ESPNOW.REMOTE") {
            _remote_control_app = props.appID;
        }
    }
    stackpet::remote::RemoteLink::instance().start(true);
    // Nothing went on before the boot: whatever the remote does first counts, however soon
    _navigator.reset(GetHAL().millis(), true);
}

bool AppLauncher::keeps_listening(int appID) const
{
    return std::find(_listening_apps.begin(), _listening_apps.end(), appID) != _listening_apps.end();
}

void AppLauncher::open_app(int appID, bool keepListening)
{
    // The screensaver's fetch on the router ends first (the WiFi is the app's next), the remote link resumed
    stackpet::usage::UsageFeed::instance().stop(GetHAL().millis());
    // Apps start the WiFi their own way: the remote's listening ends first, unless they keep it on (then the link
    // stops looking round the channels: they never ask it for packets)
    auto& link = stackpet::remote::RemoteLink::instance();
    if (keepListening) {
        link.settle();
    } else {
        link.release();
    }
    _app_opened = true;  // nothing more for the remote here, also later in this tick (a touch opens in _view->update())
    // The screensaver has a screen of its own (a touch ends it first, the remote does not): the previous one back
    close_screensaver();
    openApp(appID);
}

void AppLauncher::open_touched(int appID)
{
    // Touched open, the apps the remote can go home from keep listening once it has been used (since power-on, over
    // the warm reboots). Not the remote control app: it listens its own way after its role page (it should not gather
    // packets meanwhile)
    stackpet_remote_start();
    const bool listening = appID != _remote_control_app && keeps_listening(appID) &&
                           stackpet::remote::RemoteLink::instance().listening() &&
                           stackpet::remote::RemoteLink::usedSincePowerOn();
    if (listening) {
        mclog::tagInfo(getAppInfo().name, "remote in use: listening on");
    }
    open_app(appID, listening);
}

void AppLauncher::open_from_remote(int appID)
{
    const bool listening = keeps_listening(appID);
    // The remote control app as a receiver right away, no role page
    stackpet::remote::RemoteLink::instance().setDirectReceiver(appID == _remote_control_app);
    mclog::tagInfo(getAppInfo().name, "remote: open app id {}, {}", appID,
                   listening ? "listening on" : "WiFi released");
    open_app(appID, listening);
}

void AppLauncher::stackpet_remote_update()
{
    using stackpet::remote::NavAction;
    auto& link         = stackpet::remote::RemoteLink::instance();
    const uint32_t now = GetHAL().millis();
    if (_app_opened) {
        return;
    }
    stackpet_remote_start();
    if (!link.listening()) {
        return;
    }
    const auto packets = link.update(now);
    // Home already: the M5 button only ends the screensaver (its copies come while the screensaver fetches on the
    // router, the stick's packets mostly do not)
    if (link.takeHomePress(now) && _screensaver != nullptr) {
        lv_display_trigger_activity(nullptr);
    }
    if (!_view->ready()) {
        return;  // the startup animation (a cold boot): what the stick does meanwhile is not for the apps
    }
    if (_screensaver != nullptr) {
        // On the screensaver: left and right change its style (it stays), a press of the stick ends it (so does the M5
        // button, above, and a touch as ever)
        const bool pressed = std::any_of(packets.begin(), packets.end(), [](const stackpet::remote::RemotePacket& p) {
            return p.hasButtons && (p.buttons & stackpet::remote::kButtonStick) != 0;
        });
        if (pressed) {
            lv_display_trigger_activity(nullptr);
            _navigator.reset(now);
            _view->setOpenProgress(0.0f, false);
            return;
        }
        for (const auto& packet : packets) {
            _navigator.update(now, packet);
        }
        _navigator.tick(now);
        for (const auto action : _navigator.take()) {
            if (action == NavAction::Open) {
                continue;  // not without a press, and a press ended the screensaver
            }
            auto& feed = stackpet::usage::UsageFeed::instance();
            feed.setStyle(stackpet::usage::nextStyle(feed.style(), action == NavAction::Next ? 1 : -1));
            _screensaver->setStyle(feed.style());
            _screensaver_timecount = now - 1000;  // drawn at once
        }
        return;
    }
    if (!packets.empty()) {
        lv_display_trigger_activity(nullptr);  // the remote uses the screen too: no screensaver while it does
    }
    for (const auto& packet : packets) {
        _navigator.update(now, packet);
    }
    _navigator.tick(now);
    for (const auto action : _navigator.take()) {
        if (action != NavAction::Open) {
            _view->step(action == NavAction::Next ? 1 : -1);
            continue;
        }
        const int app = _view->focusedApp();
        if (app >= 0) {
            _view->setOpenProgress(0.0f, false);
            open_from_remote(app);
            return;
        }
    }
    // Held in on an app the remote cannot take home: said under the bar
    const float progress = _navigator.openProgress(now);
    const int focused    = _view->focusedApp();
    const bool touch_home =
        std::find(_touch_home_apps.begin(), _touch_home_apps.end(), focused) != _touch_home_apps.end();
    _view->setOpenProgress(progress, progress > 0.0f && touch_home);
}
