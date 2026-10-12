/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "classic_painter.h"
#include <algorithm>
#include <cmath>

namespace stackpet::face::classic {

static constexpr float kDegToRad = 3.14159265f / 180.0f;
// Above this radius arcSegment() falls back to a polyline: a flat arc covers a huge masked area and its integer
// angles get too coarse
static constexpr float kMaxArcRadius = 150.0f;

void arcPoints(Pt* out, int n, Pt c, float rx, float ry, float startDeg, float endDeg)
{
    for (int i = 0; i < n; i++) {
        float a = (startDeg + (endDeg - startDeg) * i / (n - 1)) * kDegToRad;
        out[i]  = {c.x + rx * cosf(a), c.y + ry * sinf(a)};
    }
}

void Painter::setTilt(float degrees)
{
    _tilt_deg = degrees;
    _tilt_cos = cosf(degrees * kDegToRad);
    _tilt_sin = sinf(degrees * kDegToRad);
}

Pt Painter::tilted(Pt p) const
{
    return {p.x * _tilt_cos - p.y * _tilt_sin, p.x * _tilt_sin + p.y * _tilt_cos};
}

int Painter::ax(float x) const
{
    return static_cast<int>(lroundf(_ox + x));
}

int Painter::ay(float y) const
{
    return static_cast<int>(lroundf(_oy + y));
}

void Painter::stroke(const Pt* points, int count, int width, lv_color_t color) const
{
    if (count <= 0 || width <= 0) {
        return;
    }

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.base.layer  = _layer;
    dsc.color       = color;
    dsc.width       = width;
    dsc.opa         = LV_OPA_COVER;
    dsc.round_start = 1;
    dsc.round_end   = 1;

    Pt first   = tilted(points[0]);
    int px     = ax(first.x);
    int py     = ay(first.y);
    bool drawn = false;
    for (int i = 1; i < count; i++) {
        Pt q  = tilted(points[i]);
        int x = ax(q.x);
        int y = ay(q.y);
        if (x == px && y == py) {
            continue;
        }
        dsc.p1.x = px;
        dsc.p1.y = py;
        dsc.p2.x = x;
        dsc.p2.y = y;
        lv_draw_line(_layer, &dsc);
        // Like lv_line: every segment gets a round end, which also rounds the joint with the next one
        dsc.round_start = 0;
        drawn           = true;
        px              = x;
        py              = y;
    }

    // Degenerated to a single pixel: still show a round dot
    if (!drawn) {
        fill(first, width, width, color);
    }
}

void Painter::line(Pt a, Pt b, int width, lv_color_t color) const
{
    Pt points[2] = {a, b};
    stroke(points, 2, width, color);
}

void Painter::arcSegment(Pt c, float w, float h, bool up, int width, lv_color_t color) const
{
    h = std::min(h, w / 2);
    if (h < 1.5f) {
        line({c.x - w / 2, c.y}, {c.x + w / 2, c.y}, width, color);
        return;
    }

    // Circle through both chord ends and the apex; its center is below the apex of a ∩, above the bottom of a ∪
    const float r    = (w * w / 4 + h * h) / (2 * h);
    const Pt center  = up ? Pt{c.x, c.y - h / 2 + r} : Pt{c.x, c.y + h / 2 - r};
    const float half = asinf(std::min(1.0f, w / 2 / r)) / kDegToRad;
    const float mid  = up ? 270.0f : 90.0f;

    if (r > kMaxArcRadius) {
        Pt points[9];
        arcPoints(points, 9, center, r, r, mid - half, mid + half);
        stroke(points, 9, width, color);
        return;
    }

    const Pt pc = tilted(center);
    int start   = static_cast<int>(lroundf(mid - half + _tilt_deg));
    int end     = static_cast<int>(lroundf(mid + half + _tilt_deg));
    if (start < 0) {
        start += 360;
        end += 360;
    }

    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.base.layer  = _layer;
    dsc.color       = color;
    dsc.opa         = LV_OPA_COVER;
    dsc.width       = width;
    dsc.rounded     = 1;
    dsc.center.x    = ax(pc.x);
    dsc.center.y    = ay(pc.y);
    dsc.radius      = static_cast<uint16_t>(lroundf(r + width / 2.0f));  // outer edge: the stroke is centered on r
    dsc.start_angle = start;
    dsc.end_angle   = end;
    lv_draw_arc(_layer, &dsc);
}

void Painter::fill(Pt c, float w, float h, lv_color_t color, int radius) const
{
    int iw = static_cast<int>(lroundf(w));
    int ih = static_cast<int>(lroundf(h));
    if (iw <= 0 || ih <= 0) {
        return;
    }

    lv_area_t area;
    area.x1 = ax(c.x - w / 2);
    area.y1 = ay(c.y - h / 2);
    area.x2 = area.x1 + iw - 1;
    area.y2 = area.y1 + ih - 1;

    lv_draw_fill_dsc_t dsc;
    lv_draw_fill_dsc_init(&dsc);
    dsc.base.layer = _layer;
    dsc.color      = color;
    dsc.opa        = LV_OPA_COVER;
    dsc.radius     = radius < 0 ? LV_RADIUS_CIRCLE : radius;
    lv_draw_fill(_layer, &dsc, &area);
}

void Painter::ring(Pt c, float w, float h, int width, lv_color_t color) const
{
    int iw = static_cast<int>(lroundf(w));
    int ih = static_cast<int>(lroundf(h));
    if (iw <= 0 || ih <= 0) {
        return;
    }
    // Too flat for a hole: draw it filled
    if (width * 2 >= std::min(iw, ih)) {
        fill(c, w, h, color);
        return;
    }

    lv_area_t area;
    area.x1 = ax(c.x - w / 2);
    area.y1 = ay(c.y - h / 2);
    area.x2 = area.x1 + iw - 1;
    area.y2 = area.y1 + ih - 1;

    lv_draw_border_dsc_t dsc;
    lv_draw_border_dsc_init(&dsc);
    dsc.base.layer = _layer;
    dsc.color      = color;
    dsc.opa        = LV_OPA_COVER;
    dsc.width      = width;
    dsc.radius     = LV_RADIUS_CIRCLE;
    dsc.side       = LV_BORDER_SIDE_FULL;
    lv_draw_border(_layer, &dsc, &area);
}

void Painter::triangle(Pt a, Pt b, Pt c, lv_color_t color) const
{
    lv_draw_triangle_dsc_t dsc;
    lv_draw_triangle_dsc_init(&dsc);
    dsc.base.layer = _layer;
    dsc.color      = color;
    dsc.opa        = LV_OPA_COVER;
    dsc.p[0].x     = ax(a.x);
    dsc.p[0].y     = ay(a.y);
    dsc.p[1].x     = ax(b.x);
    dsc.p[1].y     = ay(b.y);
    dsc.p[2].x     = ax(c.x);
    dsc.p[2].y     = ay(c.y);
    lv_draw_triangle(_layer, &dsc);
}

lv_area_t Painter::clipY(float y1, float y2) const
{
    // Setting the layer clip before adding draw tasks is how LVGL's own widgets clip (see lv_draw.h)
    lv_area_t previous = _layer->_clip_area;
    lv_area_t clip     = previous;
    clip.y1            = std::max<int32_t>(clip.y1, ay(y1));
    clip.y2            = std::min<int32_t>(clip.y2, ay(y2));
    _layer->_clip_area = clip;
    return previous;
}

void Painter::restoreClip(const lv_area_t& previous) const
{
    _layer->_clip_area = previous;
}

/* --------------------------------- shapes --------------------------------- */

// Tangent point of the line from p to the circle (center, r), on the side given by sign (+1 / -1), pulled a bit
// into the circle so the triangle edge that joins it is fully covered by the anti-aliased circle
static Pt tangent_point(Pt p, Pt center, float r, float sign)
{
    float dx = p.x - center.x;
    float dy = p.y - center.y;
    float d  = sqrtf(dx * dx + dy * dy);
    float ux = dx / d;
    float uy = dy / d;
    float a  = r / d;
    float b  = sqrtf(d * d - r * r) / d * sign;
    Pt t     = {center.x + r * (a * ux - b * uy), center.y + r * (a * uy + b * ux)};
    // 15 % towards the center: the edge leaves the circle a little below the exact tangent point (invisible)
    return {t.x + (center.x - t.x) * 0.15f, t.y + (center.y - t.y) * 0.15f};
}

void Painter::heart(Pt c, float w, float sy, lv_color_t color) const
{
    // Classic construction: lower half of a 45° rotated square + two round lobes on its upper sides
    const float l  = w / 1.767f;          // square side (lobes slightly larger than half the side)
    const float r  = 0.53f * l;           // lobe radius
    const float d  = 0.0733f * l;         // vertical offset that centers the heart on c
    const float lx = 0.35355f * l;        // lobe center x
    const float ly = d - 0.35355f * l;    // lobe center y
    const float by = 0.70711f * l + d;    // bottom tip y
    auto squash    = [&](float y) { return c.y + y * sy; };

    Pt tip   = {c.x, squash(by)};
    Pt left  = {c.x - lx, squash(ly)};
    Pt right = {c.x + lx, squash(ly)};
    // Outer tangent points, computed on the unsquashed shape, then squashed
    Pt tl = tangent_point({0, by}, {-lx, ly}, r, 1);
    Pt tr = tangent_point({0, by}, {lx, ly}, r, -1);
    tl    = {c.x + tl.x, squash(tl.y)};
    tr    = {c.x + tr.x, squash(tr.y)};

    float lobe_h = std::max(2 * r * sy, 6.0f);  // collapsed: a line as thick as the other eyes'
    fill(left, 2 * r, lobe_h, color);
    fill(right, 2 * r, lobe_h, color);
    if (sy > 0.08f) {
        triangle(tl, tr, tip, color);
    }
}

void Painter::tear(Pt c, float r, lv_color_t color) const
{
    const float d  = 2.3f * r;  // circle center to tip
    const float cy = c.y + (d - r) / 2;
    Pt center      = {c.x, cy};
    Pt tip         = {c.x, cy - d};
    fill(center, 2 * r, 2 * r, color);
    triangle(tangent_point(tip, center, r, 1), tangent_point(tip, center, r, -1), tip, color);
}

void Painter::angerMark(Pt c, float size, int width, lv_color_t color) const
{
    const float s   = size / 2 - width / 2.0f;
    const float gap = size * 0.13f;
    const float r   = s - gap;
    Pt points[6];
    for (int sx = -1; sx <= 1; sx += 2) {
        for (int sy = -1; sy <= 1; sy += 2) {
            // Quarter arc around the outer corner, bulging towards the center
            Pt corner = {c.x + sx * s, c.y + sy * s};
            for (int i = 0; i < 6; i++) {
                float a   = 90.0f * i / 5 * kDegToRad;
                points[i] = {corner.x - sx * r * cosf(a), corner.y - sy * r * sinf(a)};
            }
            stroke(points, 6, width, color);
        }
    }
}

void Painter::question(Pt c, float h, int width, lv_color_t color) const
{
    const float k = h / 34.0f;
    Pt points[13];
    // Hook: arc over the top, then down into the stem
    arcPoints(points, 10, {c.x, c.y - 8 * k}, 8.5f * k, 8.5f * k, 195, 400);
    points[10] = {c.x + 1.0f * k, c.y + 1.5f * k};
    points[11] = {c.x, c.y + 4 * k};
    points[12] = {c.x, c.y + 6.5f * k};
    stroke(points, 13, width, color);
    float dot = width + 1.5f * k;
    fill({c.x, c.y + 14 * k}, dot, dot, color);
}

void Painter::zzz(Pt c, float s, lv_color_t color) const
{
    struct Z {
        float dx, dy, scale;
    };
    static constexpr Z kZs[] = {{0, 0, 1.0f}, {0.98f, -0.80f, 0.72f}, {1.72f, -1.42f, 0.5f}};
    for (const auto& z : kZs) {
        float size   = s * z.scale;
        float x      = c.x + s * z.dx - size / 2;
        float y      = c.y + s * z.dy - size / 2;
        Pt points[4] = {{x, y}, {x + size, y}, {x, y + size}, {x + size, y + size}};
        stroke(points, 4, std::max(3, static_cast<int>(lroundf(size / 3.6f))), color);
    }
}

void Painter::bowl(float cx, float top, float w, float h, lv_color_t color, bool inverted) const
{
    if (h < 1 || w < 1) {
        return;
    }
    // A rounded rect taller than the bowl, cut at the flat edge. LVGL clamps the radius to half the shorter
    // side, so the rect must be at least 2 * r high for the round side to get the full radius.
    const float r = std::min(h, w / 2);
    if (!inverted) {
        lv_area_t previous = clipY(top, top + h + 1);
        fill({cx, top + (h - r) / 2}, w, h + r, color, static_cast<int>(lroundf(r)));
        restoreClip(previous);
    } else {
        lv_area_t previous = clipY(top - h - 1, top - 1);
        fill({cx, top - (h - r) / 2}, w, h + r, color, static_cast<int>(lroundf(r)));
        restoreClip(previous);
    }
}

}  // namespace stackpet::face::classic
