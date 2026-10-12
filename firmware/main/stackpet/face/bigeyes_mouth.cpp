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
static constexpr uint32_t kOpenMs        = 70;   // SpeakingModifier flips the weight every 180 ms
static constexpr uint32_t kLingerMs      = 450;  // closed phases of speech keep the mouth on screen
static constexpr uint32_t kColorMs       = 260;
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

BigEyesMouth::BigEyesMouth(lv_obj_t* parent)
{
    _part.create(parent);
    _color = _color_from = _color_to = lv_color_hex(emotionColor(Emotion::Neutral));
    _timer                           = lv_timer_create(on_timer, kTimerPeriodMs, this);
    refresh();
}

BigEyesMouth::~BigEyesMouth()
{
    if (_timer) {
        lv_timer_delete(_timer);
        _timer = nullptr;
    }
}

void BigEyesMouth::on_timer(lv_timer_t* timer)
{
    static_cast<BigEyesMouth*>(lv_timer_get_user_data(timer))->refresh();
}

void BigEyesMouth::kick()
{
    if (_timer && _paused) {
        lv_timer_resume(_timer);
        _paused = false;
    }
}

void BigEyesMouth::setPosition(const uitk::Vector2i& position)
{
    Element::setPosition(position);
    kick();
    refresh();
}

void BigEyesMouth::setWeight(int weight)
{
    Feature::setWeight(weight);
    const uint32_t now = now_ms();
    if (_weight > 0) {
        _talking      = true;
        _last_talk_ms = now;
    }
    _open_from = lerpf(_open_from, _open_to, easeOut(progress(now, _open_t0, _open_ms)));
    _open_to   = clamp01(_weight / 80.0f);
    _open_t0   = now;
    _open_ms   = kOpenMs;
    kick();
    refresh();
}

void BigEyesMouth::setRotation(int rotation)
{
    Element::setRotation(rotation);
    kick();
    refresh();
}

void BigEyesMouth::setEmotion(const Emotion& emotion)
{
    if (getIgnoreEmotion() || emotion == _emotion) {
        return;
    }
    refresh();
    _emotion    = emotion;
    _color_from = _color;
    _color_to   = lv_color_hex(emotionColor(emotion));
    _color_t0   = now_ms();
    _color_ms   = kColorMs;
    kick();
    refresh();
}

void BigEyesMouth::setVisible(bool visible)
{
    Element::setVisible(visible);
    kick();
    refresh();
}

void BigEyesMouth::refresh()
{
    const uint32_t now = now_ms();
    bool busy          = false;

    const float to = progress(now, _open_t0, _open_ms);
    _open          = lerpf(_open_from, _open_to, easeOut(to));
    busy |= to < 1.0f;

    const float tc = progress(now, _color_t0, _color_ms);
    _color         = mixColor(_color_from, _color_to, easeInOut(tc));
    busy |= tc < 1.0f;

    // Talking = weight > 0 now, or a moment ago (the closed beats of speech)
    if (_talking && _weight == 0 && now - _last_talk_ms >= kLingerMs) {
        _talking = false;
    }
    if (_talking && _weight == 0) {
        busy = true;  // wake up again to end the linger
    }
    const bool show = _visible && (_talking || _emotion == Emotion::Surprised);

    // Small tilt, IdleExpression / IMU use +-3 degrees
    float deg = (_rotation % 3600) / 10.0f;
    if (deg > 180.0f) {
        deg -= 360.0f;
    }
    float cx = 0.0f;
    if (_emotion == Emotion::Doubt) {
        // Skeptical: the mouth slides to one side and tilts
        cx = 8.0f;
        deg += 8.0f;
    }
    const float rad = deg * kPi / 180.0f;
    const float o   = _open;

    DrawList& list = _scratch;
    list.clear();
    if (show) {
        const lv_color_t c = _color;
        // Horizontal pill of size w x h, rotated with the mouth
        auto pill = [&](float w, float h) {
            const float half = std::max(0.0f, (w - h) * 0.5f);
            const float hx   = half * std::cos(rad);
            const float hy   = half * std::sin(rad);
            list.line(cx - hx, -hy, cx + hx, hy, h, c);
        };
        // Arc around a centre offset (ox, oy) from the mouth centre, rotated with the mouth
        auto arc = [&](float ox, float oy, float radius, float width, float start, float end, bool rounded) {
            const float rx = ox * std::cos(rad) - oy * std::sin(rad);
            const float ry = ox * std::sin(rad) + oy * std::cos(rad);
            list.arc(cx + rx, ry, radius, width, start + deg, end + deg, rounded, c);
        };

        switch (_emotion) {
            case Emotion::Happy:
            case Emotion::Shy:
            case Emotion::Love:
            case Emotion::Laughing: {
                if (o < 0.15f) {
                    arc(0.0f, -9.0f, 17.0f, 6.0f, 30.0f, 150.0f, true);  // closed smile
                } else {
                    // Open "D": a half disc, flat edge on top
                    const float r = _emotion == Emotion::Laughing ? 13.0f + 13.0f * o : 10.0f + 11.0f * o;
                    arc(0.0f, -r * 0.5f, r, r, 0.0f, 180.0f, false);
                }
                break;
            }
            case Emotion::Sad:
                if (o < 0.15f) {
                    arc(0.0f, 11.0f, 17.0f, 6.0f, 210.0f, 330.0f, true);  // closed frown
                } else {
                    pill(16.0f + 6.0f * o, 5.0f + 12.0f * o);
                }
                break;
            case Emotion::Surprised:
                arc(0.0f, 0.0f, 11.0f + 7.0f * o, 6.0f, 0.0f, 360.0f, false);  // "o"
                break;
            case Emotion::Angry:
                pill(36.0f + 10.0f * o, 8.0f + 16.0f * o);
                break;
            case Emotion::Sleepy:
                pill(16.0f + 6.0f * o, 5.0f + 11.0f * o);
                break;
            case Emotion::Doubt:
                pill(20.0f + 8.0f * o, 6.0f + 14.0f * o);
                break;
            case Emotion::Neutral:
            default:
                pill(28.0f + 10.0f * o, 7.0f + 19.0f * o);
                break;
        }
    }

    const int32_t ax = 160 + static_cast<int32_t>(std::lround(_position.x * kMoveX / 100.0f));
    const int32_t ay = 120 + kMouthY + static_cast<int32_t>(std::lround(_position.y * kMoveY / 100.0f));
    _part.update(ax, ay, list);

    if (!busy && _timer && !_paused) {
        lv_timer_pause(_timer);
        _paused = true;
    }
}

}  // namespace stackpet::face::bigeyes
