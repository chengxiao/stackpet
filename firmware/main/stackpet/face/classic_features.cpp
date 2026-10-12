/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
// No HAL / NVS here: also compiled by the host face preview renderer (tools/face_preview)
#include "classic_features.h"
#include <algorithm>
#include <cmath>

using stackchan::avatar::Emotion;

namespace stackpet::face::classic {

namespace {

// Emotion transition timeline in ms: close, hold the collapsed line (shape swapped), open
constexpr int32_t kCloseMs      = 60;
constexpr int32_t kHoldMs       = 15;
constexpr int32_t kOpenMs       = 80;
constexpr int32_t kTransitionMs = kCloseMs + kHoldMs + kOpenMs;

// Layout, relative to the screen center: close to the official eye / mouth spots, same ±16 px travel
constexpr int kEyeObjSize = 136;
constexpr int kEyeX       = 76;
constexpr int kEyeY       = -16;
constexpr int kMouthObjW  = 160;
constexpr int kMouthObjH  = 100;
constexpr int kMouthY     = 30;
constexpr int kTravel     = 16;

constexpr int kEyeStroke   = 8;
constexpr int kBrowStroke  = 7;
constexpr int kMouthStroke = 7;

// Speaking: below this mouth weight the emotion's resting mouth is shown
constexpr float kTalkThreshold = 0.12f;

const lv_color_t kWhite  = lv_color_hex(0xFFFFFF);
const lv_color_t kRed    = lv_color_hex(0xFF3B3B);  // anger mark
const lv_color_t kBlue   = lv_color_hex(0x4DA8FF);  // tear
const lv_color_t kPink   = lv_color_hex(0xFF7EA8);  // blush
const lv_color_t kHeart  = lv_color_hex(0xFF3C6E);
const lv_color_t kTongue = lv_color_hex(0xFF6F96);

float smooth(float x)
{
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3 - 2 * x);
}

float morph_at(int32_t ms)
{
    if (ms < kCloseMs) {
        return 1 - smooth(static_cast<float>(ms) / kCloseMs);
    }
    if (ms < kCloseMs + kHoldMs) {
        return 0;
    }
    return smooth(static_cast<float>(ms - kCloseMs - kHoldMs) / kOpenMs);
}

int travel(int position)
{
    return uitk::map_range(position, -100, 100, -kTravel, kTravel);
}

}  // namespace

/* ------------------------------------------------------------------------- */
/*                               ClassicFeature                              */
/* ------------------------------------------------------------------------- */

ClassicFeature::ClassicFeature(lv_obj_t* parent, int width, int height)
{
    _obj = lv_obj_create(parent);
    lv_obj_remove_style_all(_obj);
    lv_obj_remove_flag(_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(_obj, width, height);
    lv_obj_set_align(_obj, LV_ALIGN_CENTER);
    lv_obj_add_event_cb(_obj, draw_event_cb, LV_EVENT_DRAW_MAIN, this);
    lv_obj_add_event_cb(_obj, delete_event_cb, LV_EVENT_DELETE, this);
}

ClassicFeature::~ClassicFeature()
{
    lv_anim_delete(this, anim_exec_cb);
    if (_obj) {
        lv_obj_t* obj = _obj;
        _obj          = nullptr;
        lv_obj_delete(obj);
    }
}

void ClassicFeature::draw_event_cb(lv_event_t* e)
{
    auto* self = static_cast<ClassicFeature*>(lv_event_get_user_data(e));
    lv_area_t coords;
    lv_obj_get_coords(lv_event_get_current_target_obj(e), &coords);
    Painter painter(lv_event_get_layer(e), (coords.x1 + coords.x2 + 1) / 2.0f, (coords.y1 + coords.y2 + 1) / 2.0f);
    self->draw(painter);
}

void ClassicFeature::delete_event_cb(lv_event_t* e)
{
    auto* self = static_cast<ClassicFeature*>(lv_event_get_user_data(e));
    self->_obj = nullptr;
    lv_anim_delete(self, anim_exec_cb);
}

void ClassicFeature::place(int x, int y)
{
    if (_obj) {
        lv_obj_set_pos(_obj, x, y);
    }
}

void ClassicFeature::redraw()
{
    if (_obj) {
        lv_obj_invalidate(_obj);
    }
}

void ClassicFeature::setEmotion(const Emotion& emotion)
{
    if (getIgnoreEmotion() || emotion == _target) {
        return;
    }
    _target = emotion;
    // Still closing: the new target is swapped in at the collapsed point
    if (!_swapped) {
        return;
    }
    // Idle (morph 1) or opening: close again from the current openness, so nothing jumps
    start_transition(static_cast<int32_t>(lroundf((1 - _morph) * kCloseMs)));
}

Emotion ClassicFeature::getEmotion() const
{
    return _target;
}

void ClassicFeature::setVisible(bool visible)
{
    Element::setVisible(visible);
    if (_obj) {
        if (visible) {
            lv_obj_remove_flag(_obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(_obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void ClassicFeature::start_transition(int32_t fromMs)
{
    _swapped = false;
    lv_anim_delete(this, anim_exec_cb);

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, this);
    lv_anim_set_exec_cb(&anim, anim_exec_cb);
    lv_anim_set_values(&anim, fromMs, kTransitionMs);
    lv_anim_set_duration(&anim, kTransitionMs - fromMs);
    lv_anim_start(&anim);
}

void ClassicFeature::anim_exec_cb(void* var, int32_t value)
{
    auto* self   = static_cast<ClassicFeature*>(var);
    bool swapped = false;
    if (!self->_swapped && value >= kCloseMs) {
        self->_swapped = true;
        swapped        = self->_shown != self->_target;
        self->_shown   = self->_target;
    }

    float morph = morph_at(value);
    if (morph != self->_morph || swapped) {
        self->_morph = morph;
        self->redraw();
    }
}

/* ------------------------------------------------------------------------- */
/*                                 ClassicEye                                */
/* ------------------------------------------------------------------------- */

ClassicEye::ClassicEye(lv_obj_t* parent, bool isLeftEye)
    : ClassicFeature(parent, kEyeObjSize, kEyeObjSize), _is_left_eye(isLeftEye)
{
    Feature::setWeight(100);
    setPosition(_position);
}

void ClassicEye::setPosition(const uitk::Vector2i& position)
{
    Element::setPosition(position);
    place((_is_left_eye ? -kEyeX : kEyeX) + travel(_position.x), kEyeY + travel(_position.y));
}

void ClassicEye::setWeight(int weight)
{
    int previous = getWeight();
    Feature::setWeight(weight);
    if (getWeight() != previous) {
        redraw();
    }
}

void ClassicEye::setSize(int size)
{
    int previous = getSize();
    Feature::setSize(size);
    if (getSize() != previous) {
        redraw();
    }
}

void ClassicEye::setEmotion(const Emotion& emotion)
{
    if (getIgnoreEmotion()) {
        return;
    }
    // Every emotion shape is defined at full openness, so reset it like DefaultEyes::setEmotion() does. Besides,
    // the AI agent calls BlinkModifier::resyncEyeWeights() right after setEmotion(): without this reset a resync
    // that lands in the 200 ms closed phase of a blink would latch the closed weight (25) for good.
    setWeight(100);
    ClassicFeature::setEmotion(emotion);
}

void ClassicEye::draw(Painter& p)
{
    const float m     = morph();
    const float s     = 1.0f + getSize() / 200.0f;                            // eye scale, 0.5 ~ 1.5
    const float open  = std::clamp((getWeight() - 20) / 80.0f, 0.0f, 1.0f);  // blink weight 25: closed line
    const float k     = open * m;                                             // vertical squash of the eye
    const float mx    = _is_left_eye ? 1.0f : -1.0f;  // shapes are written for the left eye, +x = inner side
    const bool accent = m > 0.25f;                    // brows and accents shrink away during transitions

    // Round dot that closes into a horizontal line
    auto dot = [&](float cy, float r) {
        float d = 2 * r;
        p.fill({0, cy}, d, kEyeStroke + (d - kEyeStroke) * k, kWhite);
    };
    // Arc centered on its bounding box: up = ∩, down = ∪. Flattened into a line by k
    auto arch = [&](float cy, float w, float h, bool up, int width) {
        p.arcSegment({0, cy}, w, h * k, up, width, kWhite);
    };
    // Quadratic brow from the outer end to the inner end, scaled with m around its middle
    auto brow = [&](Pt outer, Pt ctrl, Pt inner) {
        if (!accent) {
            return;
        }
        Pt mid = {(outer.x + inner.x) / 2, (outer.y + inner.y) / 2};
        Pt points[7];
        for (int i = 0; i < 7; i++) {
            float t   = i / 6.0f;
            float u   = 1 - t;
            float x   = (u * u * outer.x + 2 * u * t * ctrl.x + t * t * inner.x) * s;
            float y   = (u * u * outer.y + 2 * u * t * ctrl.y + t * t * inner.y) * s;
            points[i] = {mx * (mid.x * s + (x - mid.x * s) * m), mid.y * s + (y - mid.y * s) * m};
        }
        p.stroke(points, 7, kBrowStroke, kWhite);
    };
    auto straight_brow = [&](Pt outer, Pt inner) {
        brow(outer, {(outer.x + inner.x) / 2, (outer.y + inner.y) / 2}, inner);
    };
    auto accent_width = [&](int width) { return std::max(2, static_cast<int>(lroundf(width * m))); };

    switch (shown()) {
        case Emotion::Neutral:
            dot(0, 16 * s);
            break;

        case Emotion::Happy:
            arch(-1 * s, 44 * s, 18 * s, true, kEyeStroke);
            break;

        case Emotion::Laughing: {
            // > < pointing at each other
            float w      = 28 * s;
            float h      = 34 * s * k;
            Pt points[3] = {{-mx * w / 2, -h / 2}, {mx * w / 2, 0}, {-mx * w / 2, h / 2}};
            p.stroke(points, 3, kEyeStroke + 1, kWhite);
            break;
        }

        case Emotion::Angry:
            dot(8 * s, 15 * s);
            straight_brow({-24, -27}, {19, -12});
            // On the left: AngryDecorator / HeartDecorator sit at the top right (108, -70)
            if (_is_left_eye && accent) {
                p.angerMark({-46, -46}, 34 * m, accent_width(6), kRed);
            }
            break;

        case Emotion::Sad:
            dot(5 * s, 15 * s);
            straight_brow({-23, -16}, {18, -31});
            if (!_is_left_eye && accent) {
                p.tear({16, 36}, 8.5f * m, kBlue);
            }
            break;

        case Emotion::Surprised:
            dot(0, 21 * s);
            brow({-18, -36}, {0, -50}, {18, -36});
            break;

        case Emotion::Doubt:
            if (_is_left_eye) {
                dot(1 * s, 16 * s);
                brow({-21, -30}, {-2, -47}, {19, -35});
            } else {
                p.line({-17 * s, 2 * s}, {17 * s, 2 * s}, kEyeStroke, kWhite);
                if (accent) {
                    p.question({46, -36}, 38 * m, accent_width(7), kWhite);
                }
            }
            break;

        case Emotion::Sleepy:
            arch(5 * s, 40 * s, 12 * s, false, kEyeStroke - 1);
            if (!_is_left_eye && accent) {
                p.zzz({30, -32}, 18 * m, kWhite);
            }
            break;

        case Emotion::Shy:
            arch(-3 * s, 36 * s, 14 * s, true, kEyeStroke - 1);
            if (accent) {
                p.fill({-mx * 4, 25}, 46 * m, 17 * m, kPink);
            }
            break;

        case Emotion::Love:
            p.heart({0, 0}, 52 * s, k, kHeart);
            break;

        default:
            dot(0, 16 * s);
            break;
    }
}

/* ------------------------------------------------------------------------- */
/*                                ClassicMouth                               */
/* ------------------------------------------------------------------------- */

ClassicMouth::ClassicMouth(lv_obj_t* parent) : ClassicFeature(parent, kMouthObjW, kMouthObjH)
{
    setPosition(_position);
}

void ClassicMouth::setPosition(const uitk::Vector2i& position)
{
    Element::setPosition(position);
    place(travel(_position.x), kMouthY + travel(_position.y));
}

void ClassicMouth::setWeight(int weight)
{
    int previous = getWeight();
    Feature::setWeight(weight);
    if (getWeight() != previous) {
        redraw();
    }
}

void ClassicMouth::setRotation(int rotation)
{
    int previous = getRotation();
    Element::setRotation(rotation);
    if (getRotation() == previous) {
        return;
    }
    // 0 ~ 3600 in 0.1°, values above 1800 tilt the other way (IdleExpression uses 3600 - n)
    int tenths = getRotation() > 1800 ? getRotation() - 3600 : getRotation();
    _tilt      = std::clamp(tenths, -300, 300) * 0.1f;
    redraw();
}

void ClassicMouth::draw(Painter& p)
{
    const float m    = morph();  // the whole mouth squashes towards its center line during transitions
    const float open = getWeight() / 100.0f;
    const bool talk  = open >= kTalkThreshold;
    auto height      = [&](float h) { return std::max<float>(kMouthStroke, h * m); };

    // Line shapes (lines, arcs, waves) follow the tilt, filled open mouths stay level
    p.setTilt(_tilt);

    auto flat_line = [&](float w) { p.line({-w / 2, 0}, {w / 2, 0}, kMouthStroke, kWhite); };
    // Arc centered on its bounding box at cy: smile = ∪, frown = ∩
    auto arc = [&](float cy, float w, float h, bool smile) {
        p.arcSegment({0, cy * m}, w, h * m, !smile, kMouthStroke, kWhite);
    };
    auto wave = [&](float cx, float cy, float w, float amplitude, float periods, int width) {
        Pt points[17];
        for (int i = 0; i < 17; i++) {
            float t   = i / 16.0f;
            points[i] = {cx - w / 2 + w * t, (cy - amplitude * sinf(t * periods * 2 * 3.14159265f)) * m};
        }
        p.stroke(points, 17, width, kWhite);
    };
    // Filled open mouth: flat top, round bottom. Tongue once it is big enough
    auto laugh = [&](float top, float w, float h) {
        float hh = height(h);
        p.bowl(0, top * m, w, hh, kWhite);
        if (hh >= 22) {
            // Pink dome: upper half of a pill, sitting on the bottom of the mouth
            float tw           = w * 0.46f;
            float th           = hh * 0.62f;
            float base         = top * m + hh * 0.8f;
            lv_area_t previous = p.clipY(base - th, base);
            p.fill({0, base}, tw, th, kTongue);
            p.restoreClip(previous);
        }
    };

    switch (shown()) {
        case Emotion::Neutral:
            if (talk) {
                p.fill({0, 0}, 50, height(9 + 28 * open), kWhite);
            } else {
                flat_line(68);
            }
            break;

        // Talking mouths hang from the corners of the resting smile, so the mouth doesn't jump while speaking
        case Emotion::Happy:
            if (talk) {
                laugh(-7, 66, 12 + 26 * open);
            } else {
                arc(2, 74, 18, true);
            }
            break;

        case Emotion::Love:
            if (talk) {
                laugh(-6, 60, 12 + 24 * open);
            } else {
                arc(2, 64, 16, true);
            }
            break;

        case Emotion::Laughing:
            laugh(-12, 84, 42 + (talk ? 10 * open : 0));
            break;

        case Emotion::Angry:
            if (talk) {
                p.bowl(0, 13 * m, 54, height(9 + 20 * open), kWhite, true);
            } else {
                // Tight "︿", clearly different from the soft sad "︵"
                Pt points[3] = {{-25, 10 * m}, {0, -5 * m}, {25, 10 * m}};
                p.stroke(points, 3, kMouthStroke, kWhite);
            }
            break;

        case Emotion::Sad:
            if (talk) {
                p.bowl(0, 13 * m, 40, height(7 + 13 * open), kWhite, true);
            } else {
                arc(6, 62, 14, false);
            }
            break;

        case Emotion::Surprised: {
            float d = 26 + (talk ? 12 * open : 0);
            p.ring({0, 2 * m}, d, height(d), kMouthStroke, kWhite);
            break;
        }

        case Emotion::Doubt:
            if (talk) {
                p.fill({8, 2 * m}, 28, height(7 + 13 * open), kWhite);
            } else {
                wave(6, 2, 56, 5, 1.5f, kMouthStroke);
            }
            break;

        case Emotion::Sleepy: {
            float d = 15 + (talk ? 8 * open : 0);
            p.ring({0, 6 * m}, d, height(d), 5, kWhite);
            break;
        }

        case Emotion::Shy:
            if (talk) {
                p.fill({0, 4 * m}, 20, height(6 + 10 * open), kWhite);
            } else {
                wave(0, 4, 30, 4, 2, kMouthStroke - 1);
            }
            break;

        default:
            flat_line(68);
            break;
    }
}

}  // namespace stackpet::face::classic
