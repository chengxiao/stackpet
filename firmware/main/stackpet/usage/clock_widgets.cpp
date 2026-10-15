/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "clock_style.h"
#include <algorithm>

namespace stackpet::usage::widgets {

namespace {

void opa_cb(void* var, int32_t v)
{
    lv_obj_set_style_opa(static_cast<lv_obj_t*>(var), static_cast<lv_opa_t>(v), 0);
}

void width_cb(void* var, int32_t v)
{
    lv_obj_set_width(static_cast<lv_obj_t*>(var), v);
}

void opacity_anim(lv_obj_t* obj, int32_t from, int32_t to, uint32_t ms, lv_anim_path_cb_t path)
{
    lv_anim_delete(obj, opa_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, opa_cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_reverse_duration(&a, ms);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, path);
    lv_anim_start(&a);
}

}  // namespace

lv_color_t rgb(uint32_t c)
{
    return lv_color_hex(c);
}

lv_obj_t* plain(lv_obj_t* parent)
{
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t* label(lv_obj_t* parent, const lv_font_t* font, uint32_t color)
{
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, rgb(color), 0);
    lv_label_set_text(l, "");
    return l;
}

lv_obj_t* dot(lv_obj_t* parent, int size, uint32_t color)
{
    lv_obj_t* d = plain(parent);
    lv_obj_set_size(d, size, size);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(d, rgb(color), 0);
    return d;
}

void setText(lv_obj_t* label, std::string& shown, const std::string& text)
{
    if (text != shown) {
        lv_label_set_text(label, text.c_str());
        shown = text;
    }
}

void setTextColor(lv_obj_t* obj, uint32_t& shown, uint32_t color)
{
    if (color != shown) {
        lv_obj_set_style_text_color(obj, rgb(color), 0);
        shown = color;
    }
}

void setBgColor(lv_obj_t* obj, uint32_t& shown, uint32_t color)
{
    if (color != shown) {
        lv_obj_set_style_bg_color(obj, rgb(color), 0);
        shown = color;
    }
}

void setHidden(lv_obj_t* obj, bool hidden)
{
    if (hidden != lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        if (hidden) {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void pulse(lv_obj_t* obj, int32_t from, int32_t to, uint32_t ms)
{
    opacity_anim(obj, from, to, ms, lv_anim_path_ease_in_out);
}

void blink(lv_obj_t* obj, uint32_t ms)
{
    opacity_anim(obj, LV_OPA_COVER, LV_OPA_TRANSP, ms, lv_anim_path_step);
}

void steady(lv_obj_t* obj, lv_opa_t opa)
{
    lv_anim_delete(obj, opa_cb);
    lv_obj_set_style_opa(obj, opa, 0);
}

lv_obj_t* bar(lv_obj_t* parent, int w, int h, uint32_t track, lv_obj_t** fill)
{
    lv_obj_t* t = plain(parent);
    lv_obj_set_size(t, w, h);
    lv_obj_set_style_radius(t, h / 2, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(t, rgb(track), 0);
    lv_obj_set_style_clip_corner(t, true, 0);
    *fill = plain(t);
    lv_obj_set_size(*fill, 0, h);
    lv_obj_set_style_radius(*fill, h / 2, 0);
    lv_obj_set_style_bg_opa(*fill, LV_OPA_COVER, 0);
    return t;
}

void setBarWidth(lv_obj_t* fill, int& shown, int target)
{
    if (target == shown) {
        return;
    }
    if (shown < 0) {
        lv_obj_set_width(fill, target);  // the first time: no growing from nothing
    } else {
        lv_anim_delete(fill, width_cb);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, fill);
        lv_anim_set_exec_cb(&a, width_cb);
        lv_anim_set_values(&a, lv_obj_get_width(fill), target);
        lv_anim_set_duration(&a, 600);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }
    shown = target;
}

int barWidth(int pct, int w, int h)
{
    if (pct <= 0) {
        return 0;
    }
    return std::max(h, w * std::min(pct, 100) / 100);
}

}  // namespace stackpet::usage::widgets
