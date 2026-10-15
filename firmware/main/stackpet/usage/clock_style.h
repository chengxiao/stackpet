/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
// StackPet: the screensaver's styles (ClockScreen shows one at a time, on a 320x240 part that floats) and the small
// LVGL helpers they share. Internal to stackpet/usage
#pragma once
#include "clock_screen.h"
#include "usage_look.h"
#include <lvgl.h>
#include <cstdint>
#include <memory>
#include <string>

LV_FONT_DECLARE(font_usage_clock_96);
LV_FONT_DECLARE(font_usage_clock_52);
LV_FONT_DECLARE(font_usage_num_26);
LV_FONT_DECLARE(font_usage_mono_72);
LV_FONT_DECLARE(font_usage_mono_16);

namespace stackpet::usage {

constexpr int kScreenW = 320;
constexpr int kScreenH = 240;

class StyleView {
public:
    virtual ~StyleView() = default;
    // Called a few times a second: only what changed is redrawn. night: nothing blinks
    virtual void show(const ClockModel& m, bool night) = 0;
    // How far the picture floats (± pixels): what fits the layout
    virtual int floatX() const = 0;
    virtual int floatY() const = 0;
};

// Each makes its objects in root (the floating 320x240 part); the screen's background is set by ClockScreen
std::unique_ptr<StyleView> makeClaudeView(lv_obj_t* root);
std::unique_ptr<StyleView> makeRingsView(lv_obj_t* root);
std::unique_ptr<StyleView> makeTerminalView(lv_obj_t* root);
std::unique_ptr<StyleView> makeSleepyView(lv_obj_t* root);

namespace widgets {

lv_color_t rgb(uint32_t c);
// An object with no theme style: no background, border or padding unless set; not scrollable or clickable
lv_obj_t* plain(lv_obj_t* parent);
lv_obj_t* label(lv_obj_t* parent, const lv_font_t* font, uint32_t color);
lv_obj_t* dot(lv_obj_t* parent, int size, uint32_t color);
// Only when it changed (styles set every frame would redraw everything)
void setText(lv_obj_t* label, std::string& shown, const std::string& text);
void setTextColor(lv_obj_t* obj, uint32_t& shown, uint32_t color);
void setBgColor(lv_obj_t* obj, uint32_t& shown, uint32_t color);
void setHidden(lv_obj_t* obj, bool hidden);
// The opacity back and forth for ever (eased), on and off (hard), or still
void pulse(lv_obj_t* obj, int32_t from, int32_t to, uint32_t ms);
void blink(lv_obj_t* obj, uint32_t ms);
void steady(lv_obj_t* obj, lv_opa_t opa);
// A rounded bar: the track, its fill inside; the fill's width grows or shrinks to target (animated after the first)
lv_obj_t* bar(lv_obj_t* parent, int w, int h, uint32_t track, lv_obj_t** fill);
void setBarWidth(lv_obj_t* fill, int& shown, int target);
// The width of pct% of w, at least h when above 0 (the round ends show)
int barWidth(int pct, int w, int h);

}  // namespace widgets
}  // namespace stackpet::usage
