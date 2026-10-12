/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "radar_view.h"
#include <fmt/format.h>
#include <algorithm>
#include <cmath>

namespace stackpet::radar {

namespace {

constexpr int kScreenW = 320;
constexpr int kScreenH = 240;
constexpr int kCx      = 160;
constexpr int kCy      = 134;
constexpr int kR       = 100;

// +yaw (the robot's left) is the right-hand side for somebody facing the robot, so it draws clockwise
constexpr float kYawToScreen = 1.0f;
constexpr float kDegToRad    = 3.14159265f / 180.0f;

constexpr uint32_t kGreen     = 0x3DFF7A;  // beam, hub, labels
constexpr uint32_t kGreenMid  = 0x23B455;  // outer ring, ticks
constexpr uint32_t kGreenDim  = 0x0F5A2A;  // inner rings, cross hair, limits
constexpr uint32_t kBlipColor = 0x9BFFB8;
constexpr uint32_t kLockColor = 0xFF3B30;
constexpr uint32_t kFriendColor = 0x3CE1FF;
constexpr uint32_t kBehindColor = 0x8A9099;  // seen, but out of the head's reach: no alarm

constexpr float kMaxTrailDeg  = 42.0f;  // afterglow length at full sweep speed
constexpr float kNearTrailDeg = 12.0f;  // the brighter part of the afterglow
constexpr float kMinAngleStep = 1.5f;   // degrees: smaller beam moves are not redrawn (~20 redraws/s at full speed)
constexpr uint32_t kFadePeriodMs = 200;
constexpr uint32_t kPingFlashMs  = 160;

constexpr uint32_t kFocusMs       = 350;  // lock box zooming in from the screen edges
constexpr uint32_t kBorderBlinkMs = 125;
constexpr int kBoxMargin          = 5;    // corner brackets sit this far outside the box

float yaw_to_deg(float yaw)
{
    return yaw * 0.1f * kYawToScreen;
}

float normalize_deg(float deg)
{
    deg = std::fmod(deg, 360.0f);
    return deg < 0.0f ? deg + 360.0f : deg;
}

lv_obj_t* make_root(lv_obj_t* parent)
{
    lv_obj_t* root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, kScreenW, kScreenH);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_CLICKABLE);
    return root;
}

lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, uint32_t color)
{
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_remove_style_all(label);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(label, "");
    return label;
}

// Position a polyline object on the bounding box of its absolute points, so only that box gets redrawn
void place_polyline(lv_obj_t* line, lv_point_precise_t* points, const float (*abs_points)[2], uint32_t count,
                    int pad)
{
    float min_x = abs_points[0][0];
    float min_y = abs_points[0][1];
    for (uint32_t i = 1; i < count; i++) {
        min_x = std::min(min_x, abs_points[i][0]);
        min_y = std::min(min_y, abs_points[i][1]);
    }
    const int ox = static_cast<int>(std::floor(min_x)) - pad;
    const int oy = static_cast<int>(std::floor(min_y)) - pad;
    for (uint32_t i = 0; i < count; i++) {
        points[i].x = static_cast<int32_t>(std::lround(abs_points[i][0])) - ox;
        points[i].y = static_cast<int32_t>(std::lround(abs_points[i][1])) - oy;
    }
    lv_obj_set_pos(line, ox, oy);
    lv_line_set_points(line, points, count);
}

}  // namespace

/* -------------------------------------------------------------------------- */
/*                                 RadarScreen                                */
/* -------------------------------------------------------------------------- */
RadarScreen::~RadarScreen()
{
    destroy();
}

lv_obj_t* RadarScreen::make_line(lv_point_precise_t* points, uint32_t count, uint32_t color, int width, lv_opa_t opa)
{
    lv_obj_t* line = lv_line_create(_root);
    lv_obj_remove_style_all(line);
    lv_obj_set_style_line_color(line, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_line_width(line, width, LV_PART_MAIN);
    lv_obj_set_style_line_opa(line, opa, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(line, true, LV_PART_MAIN);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
    lv_line_set_points(line, points, count);
    return line;
}

lv_obj_t* RadarScreen::make_ring(int radius, uint32_t color, int width, lv_opa_t opa)
{
    lv_obj_t* ring = lv_obj_create(_root);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, radius * 2, radius * 2);
    lv_obj_set_pos(ring, kCx - radius, kCy - radius);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_color(ring, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_border_width(ring, width, LV_PART_MAIN);
    lv_obj_set_style_border_opa(ring, opa, LV_PART_MAIN);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    return ring;
}

lv_obj_t* RadarScreen::make_glow(lv_opa_t opa)
{
    // A pie: an arc as wide as its radius. Only the indicator part is styled, the background arc and the knob
    // stay invisible after remove_style_all().
    lv_obj_t* arc = lv_arc_create(_root);
    lv_obj_remove_style_all(arc);
    lv_obj_set_size(arc, kR * 2, kR * 2);
    lv_obj_set_pos(arc, kCx - kR, kCy - kR);
    lv_obj_set_style_arc_width(arc, kR, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(kGreen), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, opa, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, false, LV_PART_INDICATOR);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_rotation(arc, 270);  // 0 degree = straight up, clockwise like the beam
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_angles(arc, 0, 0);
    lv_obj_add_flag(arc, LV_OBJ_FLAG_HIDDEN);
    return arc;
}

void RadarScreen::create(lv_obj_t* parent, const lv_font_t* textFont)
{
    destroy();
    _root = make_root(parent);

    // Static scope: rings, cross hair, the scan limits and 30 degree ticks
    make_ring(kR, kGreenMid, 2, LV_OPA_COVER);
    make_ring(kR * 2 / 3, kGreenDim, 1, LV_OPA_COVER);
    make_ring(kR / 3, kGreenDim, 1, LV_OPA_COVER);

    _cross_points[0][0] = {kCx - kR, kCy};
    _cross_points[0][1] = {kCx + kR, kCy};
    _cross_points[1][0] = {kCx, kCy - kR};
    _cross_points[1][1] = {kCx, kCy + kR};
    for (auto& points : _cross_points) {
        make_line(points.data(), 2, kGreenDim, 1, LV_OPA_COVER);
    }

    for (int i = 0; i < 2; i++) {
        lv_obj_t* limit = make_line(_limit_points[i].data(), 2, kGreenDim, 1, LV_OPA_COVER);
        lv_obj_set_style_line_dash_width(limit, 3, LV_PART_MAIN);
        lv_obj_set_style_line_dash_gap(limit, 4, LV_PART_MAIN);
        _limit_lines[i] = limit;
    }
    place_limits();

    for (int i = 0; i < kTickCount; i++) {
        const float a = static_cast<float>(i * 30) * kDegToRad;
        const float abs_points[2][2] = {
            {kCx + (kR - 8.0f) * std::sin(a), kCy - (kR - 8.0f) * std::cos(a)},
            {kCx + (kR - 1.0f) * std::sin(a), kCy - (kR - 1.0f) * std::cos(a)},
        };
        _ticks[i] = make_line(_tick_points[i].data(), 2, kGreenMid, 2, LV_OPA_COVER);
        place_polyline(_ticks[i], _tick_points[i].data(), abs_points, 2, 1);
    }

    // Moving parts
    _glow_far  = make_glow(46);
    _glow_near = make_glow(110);
    _beam      = make_line(_beam_points.data(), 2, kGreen, 3, LV_OPA_COVER);

    _hub = lv_obj_create(_root);
    lv_obj_remove_style_all(_hub);
    lv_obj_set_size(_hub, 9, 9);
    lv_obj_set_pos(_hub, kCx - 4, kCy - 4);
    lv_obj_set_style_radius(_hub, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(_hub, lv_color_hex(kGreen), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_hub, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(_hub, LV_OBJ_FLAG_CLICKABLE);

    for (auto& blip : _blips) {
        blip.obj = lv_obj_create(_root);
        lv_obj_remove_style_all(blip.obj);
        lv_obj_set_size(blip.obj, 10, 10);
        lv_obj_set_style_radius(blip.obj, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(blip.obj, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_remove_flag(blip.obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(blip.obj, LV_OBJ_FLAG_HIDDEN);
    }

    // Read-outs
    lv_obj_t* title = make_label(_root, &lv_font_montserrat_16, kGreen);
    lv_label_set_text(title, "RADAR");
    lv_obj_set_pos(title, 8, 6);

    _status = make_label(_root, textFont, kGreen);
    lv_obj_set_style_text_align(_status, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_width(_status, 200);
    lv_obj_align(_status, LV_ALIGN_TOP_RIGHT, -8, 3);

    lv_obj_t* bearing_caption = make_label(_root, textFont, kGreenMid);
    lv_label_set_text(bearing_caption, "方位");
    lv_obj_set_pos(bearing_caption, 8, 168);
    _bearing = make_label(_root, textFont, kGreen);
    lv_obj_set_pos(_bearing, 8, 194);

    lv_obj_t* info_caption = make_label(_root, textFont, kGreenMid);
    lv_label_set_text(info_caption, "目标");
    lv_obj_set_style_text_align(info_caption, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_width(info_caption, 60);
    lv_obj_set_pos(info_caption, kScreenW - 68, 168);
    _info = make_label(_root, textFont, kGreen);
    lv_obj_set_style_text_align(_info, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_width(_info, 60);
    lv_obj_set_pos(_info, kScreenW - 68, 194);
    lv_label_set_text(_info, "--");

    _shown_angle   = 1e9f;
    _shown_trail   = -1;
    _far_shown     = false;
    _near_shown    = false;
    _shown_bearing = 1 << 30;
    _status_text.clear();
    _status_color = 0;
    _info_text    = "--";
    setSweep(0.0f, 0.0f, 1.0f);
    setBearing(0);
}

void RadarScreen::destroy()
{
    if (_root != nullptr) {
        lv_obj_delete(_root);
    }
    _root      = nullptr;
    _glow_far  = nullptr;
    _glow_near = nullptr;
    _beam      = nullptr;
    _hub       = nullptr;
    _status    = nullptr;
    _bearing   = nullptr;
    _info      = nullptr;
    _ticks.fill(nullptr);
    for (auto& blip : _blips) {
        blip = Blip{};
    }
}

void RadarScreen::setVisible(bool visible)
{
    if (_root == nullptr) {
        return;
    }
    if (visible) {
        lv_obj_remove_flag(_root, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(_root, LV_OBJ_FLAG_HIDDEN);
    }
}

bool RadarScreen::visible() const
{
    return _root != nullptr && !lv_obj_has_flag(_root, LV_OBJ_FLAG_HIDDEN);
}

void RadarScreen::place_line(lv_obj_t* line, lv_point_precise_t* points, float angleDeg, float r0, float r1)
{
    const float a                = angleDeg * kDegToRad;
    const float abs_points[2][2] = {
        {kCx + r0 * std::sin(a), kCy - r0 * std::cos(a)},
        {kCx + r1 * std::sin(a), kCy - r1 * std::cos(a)},
    };
    place_polyline(line, points, abs_points, 2, 2);
}

void RadarScreen::set_glow(lv_obj_t* arc, float fromDeg, float toDeg, bool& shown)
{
    if (toDeg - fromDeg < 1.0f) {
        if (shown) {
            lv_obj_add_flag(arc, LV_OBJ_FLAG_HIDDEN);
            shown = false;
        }
        return;
    }
    lv_arc_set_angles(arc, static_cast<int32_t>(std::lround(normalize_deg(fromDeg))),
                      static_cast<int32_t>(std::lround(normalize_deg(toDeg))));
    if (!shown) {
        lv_obj_remove_flag(arc, LV_OBJ_FLAG_HIDDEN);
        shown = true;
    }
}

void RadarScreen::setSweep(float yaw, float velocity, float maxVelocity)
{
    if (_root == nullptr) {
        return;
    }
    const float angle = yaw_to_deg(yaw);
    const float speed = maxVelocity > 0.0f ? std::min(std::fabs(velocity) / maxVelocity, 1.0f) : 0.0f;
    const int trail   = static_cast<int>(std::lround(kMaxTrailDeg * speed / 3.0f)) * 3;  // 3 degree steps
    if (std::fabs(angle - _shown_angle) < kMinAngleStep && trail == _shown_trail) {
        return;
    }
    _shown_angle = angle;
    _shown_trail = trail;

    place_line(_beam, _beam_points.data(), angle, 5.0f, kR - 2.0f);

    // The afterglow trails behind the beam: counter-clockwise of it while it turns clockwise, and vice versa
    const float t    = static_cast<float>(trail);
    const float near = std::min(t, kNearTrailDeg);
    if (yaw_to_deg(velocity) >= 0.0f) {
        set_glow(_glow_near, angle - near, angle, _near_shown);
        set_glow(_glow_far, angle - t, angle - near, _far_shown);
    } else {
        set_glow(_glow_near, angle, angle + near, _near_shown);
        set_glow(_glow_far, angle + near, angle + t, _far_shown);
    }
}

void RadarScreen::setStatus(std::string_view text, uint32_t color)
{
    if (_status == nullptr || (_status_text == text && _status_color == color)) {
        return;
    }
    _status_text.assign(text.data(), text.size());
    _status_color = color;
    lv_obj_set_style_text_color(_status, lv_color_hex(color), LV_PART_MAIN);
    lv_label_set_text(_status, _status_text.c_str());
}

void RadarScreen::setBearing(int yaw)
{
    if (_bearing == nullptr) {
        return;
    }
    const int deg = static_cast<int>(std::lround(yaw_to_deg(static_cast<float>(yaw))));
    if (deg == _shown_bearing) {
        return;
    }
    _shown_bearing = deg;
    lv_label_set_text(_bearing, fmt::format("{:+d}°", deg).c_str());
}

void RadarScreen::setInfo(std::string_view text)
{
    if (_info == nullptr || _info_text == text) {
        return;
    }
    _info_text.assign(text.data(), text.size());
    lv_label_set_text(_info, _info_text.c_str());
}

void RadarScreen::setLimitYaw(int yaw)
{
    if (yaw == _limit_yaw) {
        return;
    }
    _limit_yaw = yaw;
    place_limits();
}

void RadarScreen::place_limits()
{
    for (int i = 0; i < 2; i++) {
        if (_limit_lines[i] == nullptr) {
            continue;
        }
        const float deg = yaw_to_deg(static_cast<float>(i == 0 ? _limit_yaw : -_limit_yaw));
        const float abs_points[2][2] = {
            {kCx + 8.0f * std::sin(deg * kDegToRad), kCy - 8.0f * std::cos(deg * kDegToRad)},
            {kCx + (kR - 2.0f) * std::sin(deg * kDegToRad), kCy - (kR - 2.0f) * std::cos(deg * kDegToRad)},
        };
        place_polyline(_limit_lines[i], _limit_points[i].data(), abs_points, 2, 1);
    }
}

void RadarScreen::addBlip(int bearing, float distance01, BlipKind kind, uint32_t nowMs)
{
    if (_root == nullptr) {
        return;
    }
    Blip& blip = _blips[_next_blip];
    _next_blip = (_next_blip + 1) % kBlipCount;

    const float a  = yaw_to_deg(static_cast<float>(bearing)) * kDegToRad;
    const float r  = std::clamp(distance01, 0.12f, 1.0f) * (kR - 6);
    const int size = kind == BlipKind::Locked ? 14 : 10;
    uint32_t color = kBlipColor;
    blip.lifeMs    = 2500;
    if (kind == BlipKind::Locked) {
        color       = kLockColor;
        blip.lifeMs = 4000;
    } else if (kind == BlipKind::Friend) {
        color = kFriendColor;
    } else if (kind == BlipKind::Behind) {
        color = kBehindColor;
    }
    lv_obj_set_size(blip.obj, size, size);
    lv_obj_set_pos(blip.obj, static_cast<int32_t>(std::lround(kCx + r * std::sin(a))) - size / 2,
                   static_cast<int32_t>(std::lround(kCy - r * std::cos(a))) - size / 2);
    lv_obj_set_style_bg_color(blip.obj, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(blip.obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(blip.obj, LV_OBJ_FLAG_HIDDEN);
    blip.bornMs   = nowMs;
    blip.active   = true;
    blip.shownOpa = LV_OPA_COVER;
}

void RadarScreen::ping(uint32_t nowMs)
{
    if (_hub == nullptr) {
        return;
    }
    _ping_until = nowMs + kPingFlashMs;
    if (!_ping_shown) {
        _ping_shown = true;
        lv_obj_set_size(_hub, 15, 15);
        lv_obj_set_pos(_hub, kCx - 7, kCy - 7);
    }
}

void RadarScreen::update(uint32_t nowMs)
{
    if (_root == nullptr) {
        return;
    }
    if (_ping_shown && static_cast<int32_t>(nowMs - _ping_until) >= 0) {
        _ping_shown = false;
        lv_obj_set_size(_hub, 9, 9);
        lv_obj_set_pos(_hub, kCx - 4, kCy - 4);
    }

    if (nowMs - _last_fade_ms < kFadePeriodMs) {
        return;
    }
    _last_fade_ms = nowMs;
    for (auto& blip : _blips) {
        if (!blip.active) {
            continue;
        }
        const uint32_t age = nowMs - blip.bornMs;
        if (age >= blip.lifeMs) {
            blip.active = false;
            lv_obj_add_flag(blip.obj, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const float left = 1.0f - static_cast<float>(age) / static_cast<float>(blip.lifeMs);
        const auto opa   = static_cast<lv_opa_t>(std::lround(40.0f + 215.0f * left));
        if (opa != blip.shownOpa) {
            blip.shownOpa = opa;
            lv_obj_set_style_bg_opa(blip.obj, opa, LV_PART_MAIN);
        }
    }
}

/* -------------------------------------------------------------------------- */
/*                                   LockView                                 */
/* -------------------------------------------------------------------------- */
LockView::~LockView()
{
    destroy();
}

void LockView::create(lv_obj_t* parent, const lv_font_t* titleFont, const lv_font_t* textFont)
{
    destroy();
    _root = make_root(parent);
    lv_obj_add_flag(_root, LV_OBJ_FLAG_HIDDEN);

    _image = lv_image_create(_root);
    lv_obj_remove_style_all(_image);
    lv_obj_set_pos(_image, 0, 0);
    lv_obj_set_size(_image, kScreenW, kScreenH);
    lv_image_set_inner_align(_image, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_remove_flag(_image, LV_OBJ_FLAG_CLICKABLE);

    // Flashing red frame around the whole screen
    _border = lv_obj_create(_root);
    lv_obj_remove_style_all(_border);
    lv_obj_set_size(_border, kScreenW, kScreenH);
    lv_obj_set_style_border_color(_border, lv_color_hex(kLockColor), LV_PART_MAIN);
    lv_obj_set_style_border_width(_border, 6, LV_PART_MAIN);
    lv_obj_set_style_border_opa(_border, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(_border, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(_border, LV_OBJ_FLAG_HIDDEN);

    // Cross hair in the middle: where the head aims
    _cross_points[0][0] = {0, 8};
    _cross_points[0][1] = {17, 8};
    _cross_points[1][0] = {8, 0};
    _cross_points[1][1] = {8, 17};
    for (size_t i = 0; i < _cross.size(); i++) {
        _cross[i] = lv_line_create(_root);
        lv_obj_remove_style_all(_cross[i]);
        lv_obj_set_style_line_color(_cross[i], lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_line_width(_cross[i], 2, LV_PART_MAIN);
        lv_obj_set_style_line_opa(_cross[i], LV_OPA_70, LV_PART_MAIN);
        lv_obj_set_pos(_cross[i], kScreenW / 2 - 8, kScreenH / 2 - 8);
        lv_line_set_points(_cross[i], _cross_points[i].data(), 2);
    }

    _box = lv_obj_create(_root);
    lv_obj_remove_style_all(_box);
    lv_obj_set_style_border_color(_box, lv_color_hex(kLockColor), LV_PART_MAIN);
    lv_obj_set_style_border_width(_box, 2, LV_PART_MAIN);
    lv_obj_set_style_border_opa(_box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(_box, 3, LV_PART_MAIN);
    lv_obj_remove_flag(_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(_box, LV_OBJ_FLAG_SCROLLABLE);

    for (size_t i = 0; i < _corners.size(); i++) {
        _corners[i] = lv_line_create(_root);
        lv_obj_remove_style_all(_corners[i]);
        lv_obj_set_style_line_color(_corners[i], lv_color_hex(kLockColor), LV_PART_MAIN);
        lv_obj_set_style_line_width(_corners[i], 4, LV_PART_MAIN);
        lv_obj_set_style_line_rounded(_corners[i], true, LV_PART_MAIN);
        lv_obj_remove_flag(_corners[i], LV_OBJ_FLAG_CLICKABLE);
        lv_line_set_points(_corners[i], _corner_points[i].data(), 3);
    }

    _range = make_label(_root, textFont, 0xFFFFFF);
    lv_obj_set_style_bg_color(_range, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_range, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(_range, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(_range, 4, LV_PART_MAIN);

    _title = make_label(_root, titleFont, kLockColor);
    lv_label_set_text(_title, "锁定目标");
    lv_obj_set_style_bg_color(_title, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_title, 170, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(_title, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(_title, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(_title, 8, LV_PART_MAIN);
    lv_obj_align(_title, LV_ALIGN_TOP_MID, 0, 10);

    _dsc               = lv_image_dsc_t{};
    _dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    _dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    _has_target        = false;
    _border_on         = false;
    _title_on          = true;
    _range_text.clear();
}

void LockView::destroy()
{
    if (_root != nullptr) {
        lv_obj_delete(_root);
    }
    _root   = nullptr;
    _image  = nullptr;
    _border = nullptr;
    _box    = nullptr;
    _title  = nullptr;
    _range  = nullptr;
    _corners.fill(nullptr);
    _cross.fill(nullptr);
    _frame = RgbFrame{};
}

void LockView::show(uint32_t nowMs)
{
    if (_root == nullptr) {
        return;
    }
    _shown_ms = nowMs;
    // Start the brackets at the screen edges, they close in on the target ("focusing")
    _shown = Rect{8.0f, 8.0f, kScreenW - 8.0f, kScreenH - 8.0f};
    apply_rect(_shown);
    lv_obj_remove_flag(_root, LV_OBJ_FLAG_HIDDEN);
}

void LockView::hide()
{
    if (_root == nullptr) {
        return;
    }
    lv_obj_add_flag(_root, LV_OBJ_FLAG_HIDDEN);
    // Drop the frame: the descriptor must not point at a released buffer
    lv_image_set_src(_image, nullptr);
    _frame      = RgbFrame{};
    _has_target = false;
}

bool LockView::visible() const
{
    return _root != nullptr && !lv_obj_has_flag(_root, LV_OBJ_FLAG_HIDDEN);
}

void LockView::setFrame(const RgbFrame& frame)
{
    if (_root == nullptr || !frame.pixels || frame.width == 0 || frame.height == 0) {
        return;
    }
    // Keep the new buffer before letting the old one go; LVGL renders under the same lock, so it never sees a
    // half swapped descriptor
    _frame               = frame;
    _dsc.header.w        = frame.width;
    _dsc.header.h        = frame.height;
    _dsc.header.stride   = frame.width * 2;
    _dsc.data_size       = static_cast<uint32_t>(frame.width) * frame.height * 2;
    _dsc.data            = reinterpret_cast<const uint8_t*>(_frame.pixels.get());
    lv_image_set_src(_image, &_dsc);
}

void LockView::setTarget(float cx, float cy, float w, float h, float distanceM)
{
    // A little looser than the face itself, like a viewfinder frame
    const float bw = std::max(w * 1.25f * kScreenW, 24.0f);
    const float bh = std::max(h * 1.25f * kScreenH, 24.0f);
    const float px = (cx + 1.0f) * 0.5f * kScreenW;
    const float py = (cy + 1.0f) * 0.5f * kScreenH;
    _target        = Rect{px - bw / 2, py - bh / 2, px + bw / 2, py + bh / 2};
    _has_target    = true;

    std::string text = distanceM > 0.0f ? fmt::format("{:.1f}m", distanceM) : std::string();
    if (_range != nullptr && text != _range_text) {
        _range_text = text;
        lv_label_set_text(_range, _range_text.c_str());
    }
}

void LockView::apply_rect(const Rect& rect)
{
    const float x0 = std::clamp(rect.x0, 2.0f, kScreenW - 26.0f);
    const float y0 = std::clamp(rect.y0, 2.0f, kScreenH - 26.0f);
    const float x1 = std::clamp(rect.x1, x0 + 24.0f, kScreenW - 2.0f);
    const float y1 = std::clamp(rect.y1, y0 + 24.0f, kScreenH - 2.0f);

    lv_obj_set_pos(_box, static_cast<int32_t>(x0), static_cast<int32_t>(y0));
    lv_obj_set_size(_box, static_cast<int32_t>(x1 - x0), static_cast<int32_t>(y1 - y0));

    const float m   = static_cast<float>(kBoxMargin);
    const float arm = std::clamp(std::min(x1 - x0, y1 - y0) / 3.0f, 8.0f, 28.0f);
    const float corners[4][3][2] = {
        {{x0 - m, y0 - m + arm}, {x0 - m, y0 - m}, {x0 - m + arm, y0 - m}},  // top left
        {{x1 + m - arm, y0 - m}, {x1 + m, y0 - m}, {x1 + m, y0 - m + arm}},  // top right
        {{x0 - m, y1 + m - arm}, {x0 - m, y1 + m}, {x0 - m + arm, y1 + m}},  // bottom left
        {{x1 + m - arm, y1 + m}, {x1 + m, y1 + m}, {x1 + m, y1 + m - arm}},  // bottom right
    };
    for (size_t i = 0; i < _corners.size(); i++) {
        place_polyline(_corners[i], _corner_points[i].data(), corners[i], 3, 2);
    }

    if (_range != nullptr) {
        const bool below = y1 + m + 26.0f < kScreenH;
        lv_obj_align(_range, LV_ALIGN_TOP_LEFT, static_cast<int32_t>(x0),
                     static_cast<int32_t>(below ? y1 + m + 4.0f : y0 - m - 26.0f));
    }
}

void LockView::update(uint32_t nowMs)
{
    if (!visible()) {
        return;
    }
    const uint32_t elapsed = nowMs - _shown_ms;

    // Zoom in from the screen edges onto the target, then follow it
    const Rect start{8.0f, 8.0f, kScreenW - 8.0f, kScreenH - 8.0f};
    const Rect goal = _has_target ? _target : Rect{kScreenW * 0.3f, kScreenH * 0.25f, kScreenW * 0.7f, kScreenH * 0.75f};
    Rect next       = goal;
    if (elapsed < kFocusMs) {
        const float t = static_cast<float>(elapsed) / kFocusMs;
        const float e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);  // ease out
        next.x0       = start.x0 + (goal.x0 - start.x0) * e;
        next.y0       = start.y0 + (goal.y0 - start.y0) * e;
        next.x1       = start.x1 + (goal.x1 - start.x1) * e;
        next.y1       = start.y1 + (goal.y1 - start.y1) * e;
    }
    if (std::fabs(next.x0 - _shown.x0) >= 1.0f || std::fabs(next.y0 - _shown.y0) >= 1.0f ||
        std::fabs(next.x1 - _shown.x1) >= 1.0f || std::fabs(next.y1 - _shown.y1) >= 1.0f) {
        _shown = next;
        apply_rect(_shown);
    }

    const bool border_on = (elapsed / kBorderBlinkMs) % 2 == 0;
    if (border_on != _border_on) {
        _border_on = border_on;
        if (border_on) {
            lv_obj_remove_flag(_border, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(_border, LV_OBJ_FLAG_HIDDEN);
        }
    }
    const bool title_on = elapsed % 500 < 380;
    if (title_on != _title_on) {
        _title_on = title_on;
        lv_obj_set_style_text_opa(_title, title_on ? LV_OPA_COVER : LV_OPA_20, LV_PART_MAIN);
    }
}

}  // namespace stackpet::radar
