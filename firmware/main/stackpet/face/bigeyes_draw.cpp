/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "bigeyes_draw.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace stackpet::face::bigeyes {

static int16_t px(float v)
{
    return static_cast<int16_t>(std::lround(v));
}

DrawList::Prim* DrawList::push(Type type, lv_color_t color, lv_opa_t opa)
{
    if (_count >= kMaxPrims || opa <= LV_OPA_MIN) {
        return nullptr;
    }
    Prim* p = &_prims[_count++];
    std::memset(p, 0, sizeof(Prim));
    p->type  = type;
    p->opa   = opa;
    p->color = lv_color_to_int(color);
    return p;
}

void DrawList::rect(float cx, float cy, float w, float h, float radius, lv_color_t color, lv_opa_t opa)
{
    if (w < 1.0f || h < 1.0f) {
        return;
    }
    Prim* p = push(Type::Rect, color, opa);
    if (!p) {
        return;
    }
    p->x[0] = px(cx - w * 0.5f);
    p->x[1] = static_cast<int16_t>(px(cx + w * 0.5f) - 1);
    p->y[0] = px(cy - h * 0.5f);
    p->y[1] = static_cast<int16_t>(px(cy + h * 0.5f) - 1);
    if (p->x[1] < p->x[0]) {
        p->x[1] = p->x[0];
    }
    if (p->y[1] < p->y[0]) {
        p->y[1] = p->y[0];
    }
    float max_r = std::min(p->x[1] - p->x[0] + 1, p->y[1] - p->y[0] + 1) * 0.5f;
    p->a        = px(std::clamp(radius, 0.0f, max_r));
}

void DrawList::circle(float cx, float cy, float r, lv_color_t color, lv_opa_t opa)
{
    rect(cx, cy, r * 2.0f, r * 2.0f, r, color, opa);
}

void DrawList::line(float x0, float y0, float x1, float y1, float width, lv_color_t color, bool roundStart,
                    bool roundEnd, lv_opa_t opa)
{
    if (width < 1.0f) {
        return;
    }
    int16_t ix0 = px(x0), iy0 = px(y0), ix1 = px(x1), iy1 = px(y1);
    if (ix0 == ix1 && iy0 == iy1) {
        // LVGL skips zero length lines (caps included)
        if (roundStart || roundEnd) {
            circle(ix0, iy0, width * 0.5f, color, opa);
        }
        return;
    }
    Prim* p = push(Type::Line, color, opa);
    if (!p) {
        return;
    }
    p->x[0]  = ix0;
    p->y[0]  = iy0;
    p->x[1]  = ix1;
    p->y[1]  = iy1;
    p->a     = px(width);
    p->flags = static_cast<uint8_t>((roundStart ? 1 : 0) | (roundEnd ? 2 : 0));
}

void DrawList::polyline(const float* xs, const float* ys, int n, float width, lv_color_t color, lv_opa_t opa)
{
    for (int i = 0; i + 1 < n; i++) {
        // Every joint gets one round cap (from the segment ending there), the first point gets its own
        line(xs[i], ys[i], xs[i + 1], ys[i + 1], width, color, i == 0, true, opa);
    }
}

void DrawList::triangle(float x0, float y0, float x1, float y1, float x2, float y2, lv_color_t color, lv_opa_t opa)
{
    // Skip degenerate (zero area) triangles
    float area2 = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (std::fabs(area2) < 1.0f) {
        return;
    }
    Prim* p = push(Type::Triangle, color, opa);
    if (!p) {
        return;
    }
    p->x[0] = px(x0);
    p->y[0] = px(y0);
    p->x[1] = px(x1);
    p->y[1] = px(y1);
    p->x[2] = px(x2);
    p->y[2] = px(y2);
}

void DrawList::arc(float cx, float cy, float radius, float width, float startDeg, float endDeg, bool rounded,
                   lv_color_t color, lv_opa_t opa)
{
    if (radius < 1.0f || width < 1.0f) {
        return;
    }
    Prim* p = push(Type::Arc, color, opa);
    if (!p) {
        return;
    }
    p->x[0] = px(cx);
    p->y[0] = px(cy);
    p->a    = px(radius);
    p->b    = px(std::min(width, radius));
    // Normalise to 0..359 / start..start+360 so equal arcs compare equal
    int start = static_cast<int>(std::lround(startDeg)) % 360;
    if (start < 0) {
        start += 360;
    }
    int end = static_cast<int>(std::lround(endDeg)) % 360;
    if (end < 0) {
        end += 360;
    }
    if (std::fabs(endDeg - startDeg) >= 359.5f) {
        start = 0;
        end   = 360;
    }
    p->start = static_cast<int16_t>(start);
    p->end   = static_cast<int16_t>(end);
    p->flags = rounded ? 1 : 0;
}

bool DrawList::bounds(lv_area_t& out, int margin) const
{
    if (_count == 0) {
        return false;
    }
    int32_t x1 = INT16_MAX, y1 = INT16_MAX, x2 = INT16_MIN, y2 = INT16_MIN;
    auto grow = [&](int32_t ax1, int32_t ay1, int32_t ax2, int32_t ay2) {
        x1 = std::min(x1, ax1);
        y1 = std::min(y1, ay1);
        x2 = std::max(x2, ax2);
        y2 = std::max(y2, ay2);
    };
    for (int i = 0; i < _count; i++) {
        const Prim& p = _prims[i];
        switch (p.type) {
            case Type::Rect:
                grow(p.x[0], p.y[0], p.x[1], p.y[1]);
                break;
            case Type::Line: {
                int32_t hw = p.a / 2 + 1;
                grow(std::min(p.x[0], p.x[1]) - hw, std::min(p.y[0], p.y[1]) - hw, std::max(p.x[0], p.x[1]) + hw,
                     std::max(p.y[0], p.y[1]) + hw);
                break;
            }
            case Type::Triangle:
                grow(std::min({p.x[0], p.x[1], p.x[2]}), std::min({p.y[0], p.y[1], p.y[2]}),
                     std::max({p.x[0], p.x[1], p.x[2]}), std::max({p.y[0], p.y[1], p.y[2]}));
                break;
            case Type::Arc: {
                // Exact box: the end points of both edges, the outer edge where it crosses an axis, the round caps.
                // (Nearly flat arcs have a huge radius, the full circle box would cover the whole screen.)
                const float ro  = p.a;
                const float ri  = static_cast<float>(p.a - p.b);
                const float cap = p.b * 0.5f;
                const int start = p.start;
                const int end   = p.end > p.start ? p.end : p.end + 360;
                auto point      = [&](float deg, float r, float pad) {
                    const float rad = deg * 3.14159265f / 180.0f;
                    const float x   = p.x[0] + r * std::cos(rad);
                    const float y   = p.y[0] + r * std::sin(rad);
                    grow(static_cast<int32_t>(std::floor(x - pad)), static_cast<int32_t>(std::floor(y - pad)),
                         static_cast<int32_t>(std::ceil(x + pad)), static_cast<int32_t>(std::ceil(y + pad)));
                };
                for (int deg : {start, end}) {
                    point(deg, ro, 0.0f);
                    point(deg, ri, 0.0f);
                    if (p.flags & 1) {
                        point(deg, ro - cap, cap);
                    }
                }
                for (int deg = 0; deg <= 720; deg += 90) {
                    if (deg >= start && deg <= end) {
                        point(deg, ro, 0.0f);
                    }
                }
                break;
            }
        }
    }
    out.x1 = x1 - margin;
    out.y1 = y1 - margin;
    out.x2 = x2 + margin;
    out.y2 = y2 + margin;
    return true;
}

void DrawList::draw(lv_layer_t* layer, int32_t ox, int32_t oy) const
{
    for (int i = 0; i < _count; i++) {
        const Prim& p = _prims[i];
        lv_color_t color = lv_color_hex(p.color);
        switch (p.type) {
            case Type::Rect: {
                lv_draw_rect_dsc_t dsc;
                lv_draw_rect_dsc_init(&dsc);
                dsc.bg_color = color;
                dsc.bg_opa   = p.opa;
                dsc.radius   = p.a;
                lv_area_t area = {ox + p.x[0], oy + p.y[0], ox + p.x[1], oy + p.y[1]};
                lv_draw_rect(layer, &dsc, &area);
                break;
            }
            case Type::Line: {
                lv_draw_line_dsc_t dsc;
                lv_draw_line_dsc_init(&dsc);
                dsc.color       = color;
                dsc.opa         = p.opa;
                dsc.width       = p.a;
                dsc.p1.x        = ox + p.x[0];
                dsc.p1.y        = oy + p.y[0];
                dsc.p2.x        = ox + p.x[1];
                dsc.p2.y        = oy + p.y[1];
                dsc.round_start = (p.flags & 1) ? 1 : 0;
                dsc.round_end   = (p.flags & 2) ? 1 : 0;
                lv_draw_line(layer, &dsc);
                break;
            }
            case Type::Triangle: {
                lv_draw_triangle_dsc_t dsc;
                lv_draw_triangle_dsc_init(&dsc);
                dsc.color = color;
                dsc.opa   = p.opa;
                for (int k = 0; k < 3; k++) {
                    dsc.p[k].x = ox + p.x[k];
                    dsc.p[k].y = oy + p.y[k];
                }
                lv_draw_triangle(layer, &dsc);
                break;
            }
            case Type::Arc: {
                lv_draw_arc_dsc_t dsc;
                lv_draw_arc_dsc_init(&dsc);
                dsc.color       = color;
                dsc.opa         = p.opa;
                dsc.center.x    = ox + p.x[0];
                dsc.center.y    = oy + p.y[0];
                dsc.radius      = static_cast<uint16_t>(p.a);
                dsc.width       = p.b;
                dsc.start_angle = p.start;
                dsc.end_angle   = p.end;
                dsc.rounded     = (p.flags & 1) ? 1 : 0;
                lv_draw_arc(layer, &dsc);
                break;
            }
        }
    }
}

lv_color_t mixColor(lv_color_t from, lv_color_t to, float t)
{
    if (t <= 0.0f) {
        return from;
    }
    if (t >= 1.0f) {
        return to;
    }
    float r = from.red + (to.red - from.red) * t;
    float g = from.green + (to.green - from.green) * t;
    float b = from.blue + (to.blue - from.blue) * t;
    const float peak   = std::max({r, g, b});
    const float target = std::max({from.red, from.green, from.blue}) +
                         (std::max({to.red, to.green, to.blue}) - std::max({from.red, from.green, from.blue})) * t;
    if (peak > 1.0f) {
        const float k = target / peak;
        r *= k;
        g *= k;
        b *= k;
    }
    auto to_u8 = [](float v) { return static_cast<uint8_t>(std::clamp(v + 0.5f, 0.0f, 255.0f)); };
    return lv_color_make(to_u8(r), to_u8(g), to_u8(b));
}

bool DrawList::operator==(const DrawList& other) const
{
    return _count == other._count && std::memcmp(_prims, other._prims, sizeof(Prim) * _count) == 0;
}

}  // namespace stackpet::face::bigeyes
