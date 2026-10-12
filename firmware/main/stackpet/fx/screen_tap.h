/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <lvgl.h>
#include <atomic>

namespace stackpet::fx {

/**
 * Screen tap detector on an LVGL object (usually the avatar panel). LVGL callbacks run in the LVGL
 * thread; poll() is for the app main loop. Touches that start near the top or bottom edge are ignored,
 * because those are the status bar (swipe down) and home indicator (swipe up) gestures.
 */
class ScreenTap {
public:
    // Call with the LVGL lock held. The object must not outlive this ScreenTap.
    void attach(lv_obj_t* target);
    // Returns true once per tap, with the release position in screen pixels.
    bool poll(int& x, int& y);

private:
    static void on_event(lv_event_t* e);

    static constexpr int kEdgeGuardPx = 28;

    std::atomic<bool> _pending{false};
    std::atomic<bool> _press_valid{false};
    std::atomic<int> _x{0};
    std::atomic<int> _y{0};
};

}  // namespace stackpet::fx
