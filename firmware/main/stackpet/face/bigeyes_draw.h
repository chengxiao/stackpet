/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <lvgl.h>
#include <cstdint>

namespace stackpet::face::bigeyes {

/**
 * A tiny retained draw list for the big-eyes skin. Geometry is built in float, relative to an anchor point (the
 * feature centre), rounded to whole pixels and replayed in the LVGL draw event of a single object. Keeping the
 * geometry separate from the drawing lets a feature size its object to the exact bounds (tight invalidation) and
 * skip the redraw entirely when a rebuilt list is identical to the previous one.
 */
class DrawList {
public:
    static constexpr int kMaxPrims = 24;

    void clear()
    {
        _count = 0;
    }
    bool empty() const
    {
        return _count == 0;
    }

    // Filled rounded rectangle centred on (cx, cy). radius is clamped to half the shorter side.
    void rect(float cx, float cy, float w, float h, float radius, lv_color_t color, lv_opa_t opa = LV_OPA_COVER);
    void circle(float cx, float cy, float r, lv_color_t color, lv_opa_t opa = LV_OPA_COVER);
    // Thick line segment with optional round caps. A zero length segment with round caps becomes a dot.
    void line(float x0, float y0, float x1, float y1, float width, lv_color_t color, bool roundStart = true,
              bool roundEnd = true, lv_opa_t opa = LV_OPA_COVER);
    // Thick open polyline with round joints and caps (one segment per point pair).
    void polyline(const float* xs, const float* ys, int n, float width, lv_color_t color,
                  lv_opa_t opa = LV_OPA_COVER);
    void triangle(float x0, float y0, float x1, float y1, float x2, float y2, lv_color_t color,
                  lv_opa_t opa = LV_OPA_COVER);
    // Circular arc, LVGL convention: angles in degrees, 0 = 3 o'clock, clockwise. radius is the outer radius and
    // width grows inwards (width >= radius fills a pie slice).
    void arc(float cx, float cy, float radius, float width, float startDeg, float endDeg, bool rounded,
             lv_color_t color, lv_opa_t opa = LV_OPA_COVER);

    // Bounds of everything in the list relative to the anchor, grown by margin (anti-aliasing). False if empty.
    bool bounds(lv_area_t& out, int margin) const;
    // Replay with the anchor at absolute screen position (ox, oy).
    void draw(lv_layer_t* layer, int32_t ox, int32_t oy) const;

    bool operator==(const DrawList& other) const;
    bool operator!=(const DrawList& other) const
    {
        return !(*this == other);
    }

private:
    enum class Type : uint8_t { Rect, Line, Triangle, Arc };

    struct Prim {
        Type type;
        uint8_t flags;  // Line: bit0 round start, bit1 round end. Arc: bit0 rounded
        lv_opa_t opa;
        uint8_t pad;
        uint32_t color;  // 0xRRGGBB, a plain integer so whole lists can be compared
        int16_t x[3];    // Rect: x1, x2 | Line: x0, x1 | Triangle: 3 points | Arc: centre
        int16_t y[3];
        int16_t a;       // Rect: radius | Line: width | Arc: outer radius
        int16_t b;       // Arc: width
        int16_t start;   // Arc: start angle
        int16_t end;     // Arc: end angle
    };

    Prim* push(Type type, lv_color_t color, lv_opa_t opa);

    Prim _prims[kMaxPrims];
    int _count = 0;
};

// Colour tween that keeps the brightness: a plain RGB mix of two saturated colours (cyan -> orange) dips into a
// dull grey halfway, this one flashes through a light tint instead.
lv_color_t mixColor(lv_color_t from, lv_color_t to, float t);

}  // namespace stackpet::face::bigeyes
