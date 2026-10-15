/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
// StackPet: the launcher's screensaver: the clock and Claude's usage (the 5-hour and the weekly window, what Claude
// is doing on the Mac) in one of a few dark styles (usage_look.h), the colors by the usage and the state, the picture
// floating slowly. Only the screen: it shows the model it is given (the launcher fills it from the clock and
// UsageFeed), so it also renders on the host (tools/usage_preview)
#pragma once
#include "usage_look.h"
#include "usage_snapshot.h"
#include <lvgl.h>
#include <cstdint>
#include <ctime>
#include <memory>

namespace stackpet::usage {

// What the screensaver shows at a moment
struct ClockModel {
    std::tm local{};  // now, local time
    int64_t now = 0;  // now, Unix seconds
    uint32_t ms = 0;  // uptime: the float and the motion go by it
    bool haveSnapshot = false;
    Snapshot snapshot;
    FeedStatus status = FeedStatus::Waiting;
    uint32_t ageMs    = 0;  // since the snapshot came
};

class StyleView;

class ClockScreen {
public:
    // A screen of its own, loaded at once; the one that was active comes back when this one goes
    explicit ClockScreen(Style style);
    ~ClockScreen();
    ClockScreen(const ClockScreen&)            = delete;
    ClockScreen& operator=(const ClockScreen&) = delete;

    void setStyle(Style style);
    Style style() const;
    // Call it a few times a second: only what changed is redrawn. True at night (the caller dims the backlight more)
    bool show(const ClockModel& m);

private:
    lv_obj_t* _prev   = nullptr;
    lv_obj_t* _screen = nullptr;
    lv_obj_t* _root   = nullptr;  // the floating part, the style's objects in it
    std::unique_ptr<StyleView> _view;
    Style _style;
    int _dx = 0;
    int _dy = 0;
};

}  // namespace stackpet::usage
