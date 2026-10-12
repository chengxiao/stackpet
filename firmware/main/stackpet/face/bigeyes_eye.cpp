/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "bigeyes_features.h"
#include <smooth_ui_toolkit.hpp>
#include <algorithm>
#include <cmath>

using namespace stackchan::avatar;

namespace stackpet::face::bigeyes {

static constexpr uint32_t kTimerPeriodMs = 30;
static constexpr uint32_t kSquashOutMs   = 100;  // close into the bar (scaled by how open the eye still is)
static constexpr uint32_t kSquashInMs    = 170;  // pop the new shape open
static constexpr uint32_t kMorphMs       = 220;
static constexpr uint32_t kBlinkCloseMs  = 60;
static constexpr uint32_t kBlinkOpenMs   = 100;
static constexpr float kPi               = 3.14159265f;

static uint32_t now_ms()
{
    return uitk::ui_hal::get_tick();
}

static float progress(uint32_t now, uint32_t t0, uint32_t ms)
{
    if (ms == 0) {
        return 1.0f;
    }
    return clamp01(static_cast<float>(now - t0) / static_cast<float>(ms));
}

// Time inside the current cycle of a repeating effect: the first `fastCount` cycles every `fastMs`, then every
// `slowMs` (an emotion can stay on screen for minutes in AI mode, keep the redraws rare then)
static uint32_t cycle_time(uint32_t elapsed, uint32_t fastMs, uint32_t fastCount, uint32_t slowMs)
{
    const uint32_t fast_total = fastMs * fastCount;
    if (elapsed < fast_total) {
        return elapsed % fastMs;
    }
    return (elapsed - fast_total) % slowMs;
}

// Blink modifier closes to weight 25, so everything below ~25 has to read as "shut"
static float weight_to_open(int weight)
{
    return clamp01((weight - 20) / 80.0f);
}

BigEyesEye::BigEyesEye(lv_obj_t* parent, bool isLeftEye) : _is_left(isLeftEye), _in(isLeftEye ? 1 : -1)
{
    _eye.create(parent);
    _blush.create(parent);
    if (!_is_left) {
        _tear.create(parent);
        _glyph.create(parent);
    }

    _to = _from = _cur = eyeShape(Emotion::Neutral, _is_left);
    _color = _color_from = _color_to = lv_color_hex(emotionColor(Emotion::Neutral));
    _ambient_t0                      = now_ms();

    Feature::setWeight(100);
    _timer = lv_timer_create(on_timer, kTimerPeriodMs, this);
    refresh();
}

BigEyesEye::~BigEyesEye()
{
    if (_timer) {
        lv_timer_delete(_timer);
        _timer = nullptr;
    }
}

void BigEyesEye::on_timer(lv_timer_t* timer)
{
    static_cast<BigEyesEye*>(lv_timer_get_user_data(timer))->refresh();
}

void BigEyesEye::kick()
{
    if (_timer && _paused) {
        lv_timer_resume(_timer);
        _paused = false;
    }
}

void BigEyesEye::setPosition(const uitk::Vector2i& position)
{
    Element::setPosition(position);
    kick();
    refresh();
}

void BigEyesEye::setWeight(int weight)
{
    Feature::setWeight(weight);
    if (weight_to_open(_weight) == _open_to) {
        return;  // e.g. setEmotion() re-opening an eye that is open already
    }
    const uint32_t now = now_ms();
    // Continue from what is on screen right now
    _open_from = lerpf(_open_from, _open_to, easeOut(progress(now, _open_t0, _open_ms)));
    _open_to   = weight_to_open(_weight);
    _open_t0   = now;
    _open_ms   = _open_to < _open_from ? kBlinkCloseMs : kBlinkOpenMs;
    kick();
    refresh();
}

void BigEyesEye::setSize(int size)
{
    Feature::setSize(size);
    kick();
    refresh();
}

void BigEyesEye::setRotation(int rotation)
{
    Element::setRotation(rotation);
    kick();
    refresh();
}

void BigEyesEye::setVisible(bool visible)
{
    Element::setVisible(visible);
    kick();
    refresh();
}

void BigEyesEye::setEmotion(const Emotion& emotion)
{
    if (getIgnoreEmotion()) {
        return;
    }
    // Every emotion shape is defined at full openness, so re-open the eye like DefaultEyes::setEmotion() does. The AI
    // agent calls BlinkModifier::resyncEyeWeights() right after setEmotion(): without this reset a resync that lands in
    // the 200 ms closed phase of a blink would latch the closed weight (25) for good.
    setWeight(100);
    if (emotion == _emotion) {
        return;
    }
    refresh();  // bring the on-screen state up to date, the transition starts from it
    start_transition(emotion, now_ms());
    kick();
    refresh();
}

void BigEyesEye::start_transition(Emotion emotion, uint32_t now)
{
    const EyeShape target = eyeShape(emotion, _is_left);

    _emotion    = emotion;
    _color_from = _color;
    _color_to   = lv_color_hex(emotionColor(emotion));
    _color_t0   = now;

    if (_phase == Phase::SquashOut) {
        // Still closing: the swap simply picks up the newest target
        _to       = target;
        _color_ms = (_phase_ms - std::min(_phase_ms, now - _phase_t0)) + kSquashInMs;
        return;
    }

    const bool morph = _phase != Phase::SquashIn && _cur.kind == EyeKind::Body && target.kind == EyeKind::Body;
    _from     = _cur;
    _to       = target;
    _phase_t0 = now;
    if (morph) {
        _phase         = Phase::Morph;
        _overshoot     = emotion == Emotion::Surprised;
        _phase_ms      = emotion == Emotion::Sleepy ? 420 : (emotion == Emotion::Surprised ? 200 : kMorphMs);
        _color_ms      = _phase_ms + 40;
        _shown_emotion = emotion;
        _ambient_t0    = now;
    } else {
        _phase       = Phase::SquashOut;
        _squash_from = std::min(_squash, 1.0f);
        _conv_from   = _conv;
        _phase_ms    = std::max<uint32_t>(30, static_cast<uint32_t>(kSquashOutMs * _squash_from));
        _color_ms    = _phase_ms + kSquashInMs;
    }
}

void BigEyesEye::refresh()
{
    const uint32_t now = now_ms();
    bool busy          = false;  // a tween is running
    bool ambient       = false;  // the current emotion keeps moving by itself

    // Blink
    const float tb = progress(now, _open_t0, _open_ms);
    _open          = lerpf(_open_from, _open_to, easeOut(tb));
    busy |= tb < 1.0f;

    // Emotion transition
    float squash = 1.0f;
    float conv   = 0.0f;
    switch (_phase) {
        case Phase::Idle:
            _cur = _to;
            break;
        case Phase::Morph: {
            const float t = progress(now, _phase_t0, _phase_ms);
            _cur          = lerpShape(_from, _to, _overshoot ? easeOutBack(t) : easeInOut(t));
            if (t >= 1.0f) {
                _phase = Phase::Idle;
                _cur   = _to;
            } else {
                busy = true;
            }
            break;
        }
        case Phase::SquashOut: {
            const float t = progress(now, _phase_t0, _phase_ms);
            squash        = lerpf(_squash_from, 0.0f, easeIn(t));
            conv          = lerpf(_conv_from, 1.0f, easeIn(t));
            _cur          = _from;
            if (t >= 1.0f) {
                // Shut: swap in the new shape, it pops open from the bar
                _phase         = Phase::SquashIn;
                _phase_t0      = now;
                _phase_ms      = kSquashInMs;
                _cur           = _to;
                _shown_emotion = _emotion;
                _ambient_t0    = now;
            }
            busy = true;
            break;
        }
        case Phase::SquashIn: {
            const float t = progress(now, _phase_t0, _phase_ms);
            squash        = easeOutBack(t);
            conv          = 1.0f - easeOut(t);
            _cur          = _to;
            if (t >= 1.0f) {
                _phase = Phase::Idle;
                squash = 1.0f;
                conv   = 0.0f;
            } else {
                busy = true;
            }
            break;
        }
    }
    _squash = squash;
    _conv   = conv;

    // Colour
    const float tc = progress(now, _color_t0, _color_ms);
    _color         = mixColor(_color_from, _color_to, easeInOut(tc));
    busy |= tc < 1.0f;

    // Emotion specific motion of the shape on screen
    float pulse     = 1.0f;
    float dx        = 0.0f;
    float dy        = 0.0f;
    const uint32_t at = now - _ambient_t0;
    if (_phase != Phase::SquashOut) {
        switch (_shown_emotion) {
            case Emotion::Love: {
                // Heartbeat "ba-dum": 0.6 s after the hearts appear, then every 1.8 s, later every 4.5 s
                ambient          = true;
                const uint32_t t = at < 600 ? 1800 : cycle_time(at - 600, 1800, 3, 4500);
                if (t < 140) {
                    pulse = 1.0f + 0.08f * std::sin(kPi * t / 140.0f);
                } else if (t >= 200 && t < 340) {
                    pulse = 1.0f + 0.05f * std::sin(kPi * (t - 200) / 140.0f);
                }
                break;
            }
            case Emotion::Laughing: {
                // Two little hops every 1.6 s, later every 6.4 s
                ambient          = true;
                const uint32_t t = cycle_time(at, 1600, 3, 6400);
                if (t < 480) {
                    dy = -6.0f * std::fabs(std::sin(2.0f * kPi * t / 480.0f));
                }
                break;
            }
            case Emotion::Angry:
                // Shake with rage once, right after turning angry
                if (at < 400) {
                    busy = true;
                    dx   = 3.0f * std::sin(2.0f * kPi * at / 100.0f) * (1.0f - at / 400.0f);
                }
                break;
            default:
                break;
        }
    }

    // Size, plus a hint of perspective: the eye on the side we look towards gets a little bigger
    const float size_k = 1.0f + 0.2f * _size / 100.0f;
    const float look   = (_is_left ? -1.0f : 1.0f) * _position.x / 100.0f;
    const float persp  = 1.0f + 0.10f * look;
    const float vert   = 1.0f - 0.06f * std::fabs(static_cast<float>(_position.y)) / 100.0f;
    const float sx     = size_k * persp * pulse;
    const float sy     = size_k * persp * vert * pulse;

    // Eye centre in panel coordinates
    const int32_t ax = 160 + (_is_left ? -kEyeX : kEyeX) +
                       static_cast<int32_t>(std::lround(_position.x * kMoveX / 100.0f + dx));
    const int32_t ay = 120 + kEyeY + static_cast<int32_t>(std::lround(_position.y * kMoveY / 100.0f + dy));

    DrawList& list = _scratch;
    list.clear();
    if (_visible) {
        build_eye(list, _cur, _color, std::max(0.0f, _open * squash), conv, sx, sy);
    }
    _eye.update(ax, ay, list);
    apply_rotation();

    build_accessories(now, ax, ay, ambient);

    if (!busy && !ambient && _timer && !_paused) {
        lv_timer_pause(_timer);
        _paused = true;
    }
}

void BigEyesEye::apply_rotation()
{
    // Rare (JSON control only): LVGL rotates the eye object around the eye centre. Style writes invalidate the
    // object, so only touch them when something changed.
    lv_obj_t* obj  = _eye.obj();
    const int rot  = _rotation % 3600;
    if (!obj || (rot == _applied_rotation && (rot == 0 || (_eye.pivotX() == _applied_pivot_x &&
                                                           _eye.pivotY() == _applied_pivot_y)))) {
        return;
    }
    if (rot != 0) {
        lv_obj_set_style_transform_pivot_x(obj, _eye.pivotX(), 0);
        lv_obj_set_style_transform_pivot_y(obj, _eye.pivotY(), 0);
    }
    lv_obj_set_style_transform_rotation(obj, rot, 0);
    _applied_rotation = rot;
    _applied_pivot_x  = _eye.pivotX();
    _applied_pivot_y  = _eye.pivotY();
}

void BigEyesEye::build_eye(DrawList& list, const EyeShape& shape, lv_color_t color, float open, float conv, float sx,
                           float sy) const
{
    const float in = static_cast<float>(_in);
    const float sm = std::min(sx, sy);

    switch (shape.kind) {
        case EyeKind::Body: {
            const float w      = lerpf(shape.w, kBarW, conv) * sx;
            const float h_full = shape.h * sy;
            const float h      = std::max(h_full * open, kBarH * sy);
            const float cy     = lerpf(shape.dy, 0.0f, conv) * sy;
            const float radius = std::min(shape.r * sm, h * 0.5f);
            list.rect(0.0f, cy, w, h, radius, color);

            // Corner cuts: a background coloured triangle above the cut line, squashed along with the eye
            const float top   = cy - h * 0.5f;
            const float scale = sy * std::min(h / h_full, 1.2f) * (1.0f - conv);
            auto cut          = [&](float depth, bool inner) {
                if (depth < 1.0f) {
                    return;
                }
                // u: position across the eye, positive towards the face centre
                float ua = inner ? (-w * 0.5f + shape.cutStart * w) : (w * 0.5f - shape.cutStart * w);
                float ub = inner ? w * 0.5f : -w * 0.5f;
                float ya = top;
                float yb = top + depth;
                // Extend the line a little past the eye outline on both ends
                const float len = std::sqrt((ub - ua) * (ub - ua) + (yb - ya) * (yb - ya));
                const float eu  = (ub - ua) / len * 4.0f;
                const float ey  = (yb - ya) / len * 4.0f;
                ua -= eu;
                ya -= ey;
                ub += eu;
                yb += ey;
                list.triangle(in * ua, ya, in * ub, yb, in * ub, ya, lv_color_black());
            };
            cut(shape.cutIn * scale, true);
            cut(shape.cutOut * scale, false);
            break;
        }

        case EyeKind::Arc: {
            // A true circular arc. Squashing lowers its sagitta at a constant chord, so it flattens into the bar
            // while staying perfectly round (a polyline of thick segments looks faceted).
            const float half   = shape.arcHalf * kPi / 180.0f;
            const float chord  = shape.arcR * std::sin(half);           // half chord of the stroke centre line
            const float sag0   = shape.arcR * (1.0f - std::cos(half));  // apex height above the chord
            const float thick  = lerpf(shape.thick, kBarH, conv) * sm;
            const float x_conv = lerpf(1.0f, std::max(kBarW - kBarH, 1.0f) / (2.0f * chord), conv);
            const float c      = chord * x_conv * sx;
            const float sag    = sag0 * open * sy;
            const float mid    = lerpf(shape.dy, 0.0f, conv) * sy;  // halfway between apex and chord
            if (sag < 1.5f) {
                list.line(-c, mid, c, mid, thick, color);
                break;
            }
            const float radius = (c * c + sag * sag) / (2.0f * sag);
            const float a      = std::asin(std::min(1.0f, c / radius)) * 180.0f / kPi;
            const float cy     = mid - sag * 0.5f + radius;
            list.arc(0.0f, cy, radius + thick * 0.5f, thick, 270.0f - a, 270.0f + a, true, color);
            break;
        }

        case EyeKind::Chevron: {
            const float pivot  = lerpf(shape.dy, 0.0f, conv);
            const float thick  = lerpf(shape.thick, kBarH, conv) * sm;
            const float x_conv = lerpf(1.0f, (kBarW - kBarH) / (2.0f * shape.chW), conv);
            const float u[3]   = {-shape.chW, shape.chW, -shape.chW};
            const float v[3]   = {-shape.chH, 0.0f, shape.chH};
            float xs[3];
            float ys[3];
            for (int i = 0; i < 3; i++) {
                xs[i] = in * u[i] * x_conv * sx;
                ys[i] = (pivot + v[i] * open) * sy;
            }
            list.polyline(xs, ys, 3, thick, color);
            break;
        }

        case EyeKind::Heart: {
            // Two circles on the upper sides of a 45 degree diamond: width = 2.414 d, height = 2.207 d
            const float d      = shape.heartW / 2.4142f;
            const float c0     = shape.dy + 0.1035f * d;  // diamond centre, puts the visual centre at dy
            const float pivot  = lerpf(shape.dy, 0.0f, conv);
            const float x_k    = lerpf(1.0f, kBarW / shape.heartW, conv) * sx;
            const float o      = std::max(open, kBarH / (2.207f * d));  // never thinner than the bar
            const float rc     = 0.7071f * d;
            auto fy            = [&](float y) { return (pivot + (y - shape.dy) * o) * sy; };
            list.triangle(-(d + 2.0f) * x_k, fy(c0 - 2.0f), (d + 2.0f) * x_k, fy(c0 - 2.0f), 0.0f, fy(c0 + d), color);
            const float cw = 2.0f * rc * x_k;
            const float ch = 2.0f * rc * o * sy;
            list.rect(-0.5f * d * x_k, fy(c0 - 0.5f * d), cw, ch, std::min(cw, ch) * 0.5f, color);
            list.rect(0.5f * d * x_k, fy(c0 - 0.5f * d), cw, ch, std::min(cw, ch) * 0.5f, color);
            break;
        }
    }
}

void BigEyesEye::build_accessories(uint32_t now, int32_t ax, int32_t ay, bool& ambient)
{
    // Accessories belong to the shape on screen; they leave as soon as it starts closing
    const bool show       = _visible && _phase != Phase::SquashOut;
    const Emotion emotion = _shown_emotion;
    const uint32_t at     = now - _ambient_t0;
    const float out       = static_cast<float>(-_in);  // towards the screen edge
    DrawList& list        = _scratch;

    // Blush: a soft patch on each cheek, exactly where the official ShyDecorator puts its three strokes (+-108, 28,
    // see decorators/shy.cpp), so a shy face that also gets the decorator (SPIN, FREEZE, head pet) shows the strokes
    // on the blush instead of two blushes side by side
    constexpr int kShyDecoratorX = 108;
    constexpr int kShyDecoratorY = 28;
    constexpr float kBlushOut    = kShyDecoratorX - kEyeX;
    constexpr float kBlushDown   = kShyDecoratorY - kEyeY;
    list.clear();
    if (show && emotion == Emotion::Shy) {
        const float fade = clamp01(at / 220.0f);
        list.rect(out * kBlushOut, kBlushDown, 48.0f, 20.0f, 10.0f, lv_color_hex(kColorBlush),
                  static_cast<lv_opa_t>(200.0f * fade));
        if (at < 220) {
            ambient = true;
        }
    }
    _blush.update(ax, ay, list);

    if (_is_left) {
        return;
    }

    // Tear drop below the outer corner of the right eye: wells up, hangs, falls and fades, again and again
    // (the first one falls within the 2 s an emotion is shown in the FACE app)
    list.clear();
    if (show && emotion == Emotion::Sad) {
        ambient          = true;
        const uint32_t t = at % 2600;
        float grow       = 1.0f;
        float fall       = 0.0f;
        lv_opa_t opa     = LV_OPA_COVER;
        bool visible     = true;
        if (t < 250) {
            grow = 0.3f + 0.7f * easeOut(t / 250.0f);
        } else if (t >= 1250 && t < 1950) {
            const float f = (t - 1250) / 700.0f;
            fall          = 28.0f * easeIn(f);
            opa = f < 0.55f ? static_cast<lv_opa_t>(LV_OPA_COVER)
                            : static_cast<lv_opa_t>(255.0f * (1.0f - (f - 0.55f) / 0.45f));
        } else if (t >= 1950) {
            visible = false;
        }
        if (visible) {
            const float r   = 8.5f * grow;
            const float tip = 18.0f * grow;
            const float tx  = 0.884f * r;
            const float ty  = -0.467f * r;
            const float cx  = out * 31.0f;
            const float cy  = 60.0f + fall;
            const lv_color_t c = lv_color_hex(kColorTear);
            list.triangle(cx, cy - tip, cx + tx, cy + ty, cx - tx, cy + ty, c, opa);
            list.circle(cx, cy, r, c, opa);
        }
    }
    _tear.update(ax, ay, list);

    // "?" (doubt) or "Zzz" (sleepy) next to the right eye. Glyphs are never mirrored, only their position is.
    list.clear();
    if (show && emotion == Emotion::Doubt) {
        ambient        = true;
        const float gx = out * 66.0f;
        const float gy = -52.0f + ((at / 700) % 2 ? -3.0f : 0.0f);  // slow bob
        // Hook: from 9 o'clock over the top to the lower right, then down to the stem and the dot
        list.arc(gx, gy - 10.0f, 14.0f, 7.0f, 180.0f, 50.0f, true, _color);
        list.line(gx + 6.8f, gy - 2.0f, gx, gy + 6.0f, 7.0f, _color);
        list.line(gx, gy + 6.0f, gx, gy + 10.0f, 7.0f, _color);
        list.circle(gx, gy + 21.0f, 4.5f, _color);
    } else if (show && emotion == Emotion::Sleepy) {
        ambient = true;
        // (eyes closing) z, zZ, zZz, ..., then a short pause
        static constexpr int kCount[] = {0, 1, 2, 3, 3, 3};
        const int count               = kCount[(at / 450) % 6];
        struct Z {
            float x, y, size, width;
        };
        static constexpr Z kZ[] = {{48.0f, -30.0f, 18.0f, 5.0f}, {68.0f, -51.0f, 14.0f, 4.0f}, {84.0f, -68.0f, 10.0f, 3.5f}};
        const lv_color_t c      = lv_color_hex(kColorZzz);
        for (int i = 0; i < count; i++) {
            const float x  = out * kZ[i].x;
            const float y  = kZ[i].y;
            const float h  = kZ[i].size * 0.5f;
            const float xs[4] = {x - h, x + h, x - h, x + h};
            const float ys[4] = {y - h, y - h, y + h, y + h};
            list.polyline(xs, ys, 4, kZ[i].width, c);
        }
    }
    _glyph.update(ax, ay, list);
}

}  // namespace stackpet::face::bigeyes
