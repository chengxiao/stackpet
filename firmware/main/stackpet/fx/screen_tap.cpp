/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "screen_tap.h"

namespace stackpet::fx {

void ScreenTap::attach(lv_obj_t* target)
{
    if (target == nullptr) {
        return;
    }
    lv_obj_add_flag(target, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(target, on_event, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(target, on_event, LV_EVENT_CLICKED, this);
}

bool ScreenTap::poll(int& x, int& y)
{
    if (!_pending.exchange(false)) {
        return false;
    }
    x = _x.load();
    y = _y.load();
    return true;
}

void ScreenTap::on_event(lv_event_t* e)
{
    auto* self       = static_cast<ScreenTap*>(lv_event_get_user_data(e));
    lv_indev_t* indev = lv_indev_active();
    if (self == nullptr || indev == nullptr) {
        return;
    }

    lv_point_t point;
    lv_indev_get_point(indev, &point);
    const int32_t height = lv_display_get_vertical_resolution(lv_display_get_default());

    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        self->_press_valid = point.y >= kEdgeGuardPx && point.y <= height - kEdgeGuardPx;
        return;
    }

    if (lv_event_get_code(e) == LV_EVENT_CLICKED && self->_press_valid) {
        self->_x       = point.x;
        self->_y       = point.y;
        self->_pending = true;
    }
}

}  // namespace stackpet::fx
