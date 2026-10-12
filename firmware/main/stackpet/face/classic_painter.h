/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <lvgl.h>

namespace stackpet::face::classic {

struct Pt {
    float x;
    float y;
};

/**
 * Immediate mode vector drawing for the "经典加强" face, used from LV_EVENT_DRAW_MAIN. Coordinates are local to
 * the feature anchor (origin), +y points down. Everything is built from LVGL's anti-aliased line / arc / fill /
 * triangle primitives, so shapes can be squashed (blink, emotion transition) without LVGL transforms.
 */
class Painter {
public:
    Painter(lv_layer_t* layer, float originX, float originY) : _layer(layer), _ox(originX), _oy(originY)
    {
    }

    // Rotate the following line shapes (stroke / line / arcSegment) around the origin. Fills are not rotated.
    void setTilt(float degrees);

    // Polyline with round caps and joints (same look as lv_line with line_rounded)
    void stroke(const Pt* points, int count, int width, lv_color_t color) const;
    void line(Pt a, Pt b, int width, lv_color_t color) const;
    // Circular arc through both ends of a w wide chord, bulging h up (∩) or down (∪); c is the center of its
    // bounding box. Flattens into a straight line as h goes to 0.
    void arcSegment(Pt c, float w, float h, bool up, int width, lv_color_t color) const;
    // Filled rounded rect centered at c. radius < 0: pill (half of the shorter side)
    void fill(Pt c, float w, float h, lv_color_t color, int radius = -1) const;
    // Pill / circle outline centered at c
    void ring(Pt c, float w, float h, int width, lv_color_t color) const;
    void triangle(Pt a, Pt b, Pt c, lv_color_t color) const;

    // Limit the following draws to local y in [y1, y2] (intersected with the current clip). Restore with
    // restoreClip(previous).
    lv_area_t clipY(float y1, float y2) const;
    void restoreClip(const lv_area_t& previous) const;

    /* --------------------------------- shapes --------------------------------- */
    // Filled heart, w wide, squashed vertically by sy (1 = full)
    void heart(Pt c, float w, float sy, lv_color_t color) const;
    // Filled tear drop pointing up; r is the radius of its round bottom
    void tear(Pt c, float r, lv_color_t color) const;
    // Manga anger mark: four corner arcs bulging towards c; size is the full width
    void angerMark(Pt c, float size, int width, lv_color_t color) const;
    // "?" drawn with strokes, h is the total height
    void question(Pt c, float h, int width, lv_color_t color) const;
    // "Z z z" rising to the upper right, s is the size of the first Z (c = its center)
    void zzz(Pt c, float s, lv_color_t color) const;
    // Bowl: flat top edge at y = top, round bottom (inverted: flat bottom edge at y = top, round top)
    void bowl(float cx, float top, float w, float h, lv_color_t color, bool inverted = false) const;

private:
    Pt tilted(Pt p) const;
    int ax(float x) const;
    int ay(float y) const;

    lv_layer_t* _layer;
    float _ox;
    float _oy;
    float _tilt_deg = 0;
    float _tilt_cos = 1;
    float _tilt_sin = 0;
};

// Points of an elliptical arc (angles in degrees, 0 = +x, 90 = +y i.e. down), written to out[0..n-1]
void arcPoints(Pt* out, int n, Pt c, float rx, float ry, float startDeg, float endDeg);

}  // namespace stackpet::face::classic
