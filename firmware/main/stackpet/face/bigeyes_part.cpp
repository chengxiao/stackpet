/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "bigeyes_features.h"

namespace stackpet::face::bigeyes {

static constexpr int kAntiAliasMargin = 2;

Part::~Part()
{
    if (_obj) {
        lv_obj_delete(_obj);  // on_delete clears _obj
    }
}

void Part::create(lv_obj_t* parent)
{
    _obj = lv_obj_create(parent);
    lv_obj_remove_style_all(_obj);
    lv_obj_remove_flag(_obj, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE |
                                                         LV_OBJ_FLAG_CLICK_FOCUSABLE));
    lv_obj_add_flag(_obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(_obj, on_draw, LV_EVENT_DRAW_MAIN, this);
    // The parent may be deleted first (e.g. the screen gets cleaned): forget the object then
    lv_obj_add_event_cb(_obj, on_delete, LV_EVENT_DELETE, this);
    _shown = false;
}

void Part::update(int32_t anchorX, int32_t anchorY, const DrawList& list)
{
    if (!_obj) {
        return;
    }
    lv_area_t bounds;
    if (!list.bounds(bounds, kAntiAliasMargin)) {
        hide();
        return;
    }

    const bool same_bounds = bounds.x1 == _bounds.x1 && bounds.y1 == _bounds.y1 && bounds.x2 == _bounds.x2 &&
                             bounds.y2 == _bounds.y2;
    if (_shown && (!same_bounds || list != _list)) {
        // Redraw the old area; a changed position / size additionally invalidates old and new area on layout
        lv_obj_invalidate(_obj);
    }
    _list   = list;
    _bounds = bounds;
    lv_obj_set_pos(_obj, anchorX + bounds.x1, anchorY + bounds.y1);
    lv_obj_set_size(_obj, bounds.x2 - bounds.x1 + 1, bounds.y2 - bounds.y1 + 1);
    if (!_shown) {
        lv_obj_remove_flag(_obj, LV_OBJ_FLAG_HIDDEN);
        _shown = true;
    }
}

void Part::hide()
{
    if (_obj && _shown) {
        lv_obj_add_flag(_obj, LV_OBJ_FLAG_HIDDEN);
    }
    _shown = false;
}

void Part::on_draw(lv_event_t* e)
{
    auto* part = static_cast<Part*>(lv_event_get_user_data(e));
    if (!part->_obj) {
        return;
    }
    lv_area_t coords;
    lv_obj_get_coords(part->_obj, &coords);
    part->_list.draw(lv_event_get_layer(e), coords.x1 - part->_bounds.x1, coords.y1 - part->_bounds.y1);
}

void Part::on_delete(lv_event_t* e)
{
    auto* part   = static_cast<Part*>(lv_event_get_user_data(e));
    part->_obj   = nullptr;
    part->_shown = false;
}

}  // namespace stackpet::face::bigeyes
