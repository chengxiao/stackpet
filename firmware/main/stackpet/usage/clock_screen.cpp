/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "clock_screen.h"
#include "clock_style.h"

namespace stackpet::usage {

namespace {

std::unique_ptr<StyleView> make_view(Style style, lv_obj_t* root)
{
    switch (style) {
        case Style::Rings:
            return makeRingsView(root);
        case Style::Terminal:
            return makeTerminalView(root);
        case Style::Sleepy:
            return makeSleepyView(root);
        case Style::Claude:
            break;
    }
    return makeClaudeView(root);
}

}  // namespace

ClockScreen::ClockScreen(Style style) : _style(style)
{
    _prev   = lv_screen_active();
    _screen = lv_obj_create(nullptr);
    lv_obj_remove_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(_screen, 0, 0);
    lv_obj_set_style_border_width(_screen, 0, 0);
    lv_obj_set_style_bg_opa(_screen, LV_OPA_COVER, 0);
    _root = widgets::plain(_screen);
    lv_obj_set_size(_root, kScreenW, kScreenH);
    setStyle(style);
    lv_screen_load(_screen);
}

ClockScreen::~ClockScreen()
{
    _view.reset();
    if (_prev != nullptr) {
        lv_screen_load(_prev);
    }
    lv_obj_delete(_screen);  // with everything on it and their animations
}

void ClockScreen::setStyle(Style style)
{
    _view.reset();
    lv_obj_clean(_root);  // the last style's objects and their animations
    lv_obj_set_pos(_root, 0, 0);
    _dx    = 0;
    _dy    = 0;
    _style = style;
    lv_obj_set_style_bg_color(_screen, widgets::rgb(paletteFor(style).bg), 0);
    _view = make_view(style, _root);
}

Style ClockScreen::style() const
{
    return _style;
}

bool ClockScreen::show(const ClockModel& m)
{
    const bool night = clockValid(m.local) && isNight(m.local.tm_hour);
    int dx           = 0;
    int dy           = 0;
    floatOffset(m.ms, _view->floatX(), _view->floatY(), dx, dy);
    if (dx != _dx || dy != _dy) {
        lv_obj_set_pos(_root, dx, dy);
        _dx = dx;
        _dy = dy;
    }
    _view->show(m, night);
    return night;
}

}  // namespace stackpet::usage
