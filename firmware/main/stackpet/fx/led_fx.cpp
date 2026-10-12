/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "led_fx.h"
#include <hal/hal.h>
#include <algorithm>
#include <cmath>

namespace stackpet::fx {

static constexpr int kLedCount = 12;

// Physical order around the head: left row 0-5 then right row 6-11 walked backwards, so a chase loops smoothly
static constexpr int kRingOrder[kLedCount] = {0, 1, 2, 3, 4, 5, 11, 10, 9, 8, 7, 6};

static float fract(float v)
{
    return v - std::floor(v);
}

static void hsv_to_rgb(float h, float s, float v, uint8_t& r, uint8_t& g, uint8_t& b)
{
    h       = fract(h) * 6.0f;
    int i   = static_cast<int>(h);
    float f = h - i;
    float p = v * (1.0f - s);
    float q = v * (1.0f - s * f);
    float t = v * (1.0f - s * (1.0f - f));
    float rf, gf, bf;
    switch (i) {
        case 0: rf = v, gf = t, bf = p; break;
        case 1: rf = q, gf = v, bf = p; break;
        case 2: rf = p, gf = v, bf = t; break;
        case 3: rf = p, gf = q, bf = v; break;
        case 4: rf = t, gf = p, bf = v; break;
        default: rf = v, gf = p, bf = q; break;
    }
    r = static_cast<uint8_t>(rf * 255.0f);
    g = static_cast<uint8_t>(gf * 255.0f);
    b = static_cast<uint8_t>(bf * 255.0f);
}

void fill(uint8_t r, uint8_t g, uint8_t b)
{
    GetHAL().showRgbColor(r, g, b);
}

void off()
{
    GetHAL().showRgbColor(0, 0, 0);
}

void chase(float position, uint8_t r, uint8_t g, uint8_t b)
{
    float head = fract(position) * kLedCount;
    for (int i = 0; i < kLedCount; i++) {
        // Distance behind the head along the ring, in LEDs
        float behind = head - i;
        if (behind < 0) {
            behind += kLedCount;
        }
        float level = behind < 1.0f ? 1.0f : std::max(0.0f, 1.0f - (behind - 1.0f) / 3.0f) * 0.5f;
        GetHAL().setRgbColor(kRingOrder[i], r * level, g * level, b * level);
    }
    GetHAL().refreshRgb();
}

void rainbow(float phase)
{
    for (int i = 0; i < kLedCount; i++) {
        uint8_t r, g, b;
        hsv_to_rgb(phase + static_cast<float>(i) / kLedCount, 1.0f, 0.6f, r, g, b);
        GetHAL().setRgbColor(kRingOrder[i], r, g, b);
    }
    GetHAL().refreshRgb();
}

void pulse(float phase, uint8_t r, uint8_t g, uint8_t b)
{
    float level = 0.15f + 0.85f * (0.5f - 0.5f * std::cos(fract(phase) * 2.0f * static_cast<float>(M_PI)));
    GetHAL().showRgbColor(r * level, g * level, b * level);
}

void gauge(float fraction, uint8_t r, uint8_t g, uint8_t b)
{
    int lit = static_cast<int>(std::lround(std::clamp(fraction, 0.0f, 1.0f) * kLedCount));
    for (int i = 0; i < kLedCount; i++) {
        bool on = i < lit;
        GetHAL().setRgbColor(kRingOrder[i], on ? r : 0, on ? g : 0, on ? b : 0);
    }
    GetHAL().refreshRgb();
}

static uint8_t red_of(uint32_t c)
{
    return (c >> 16) & 0xFF;
}

static uint8_t green_of(uint32_t c)
{
    return (c >> 8) & 0xFF;
}

static uint8_t blue_of(uint32_t c)
{
    return c & 0xFF;
}

void LedAnimator::set(LedMode mode, uint32_t color, uint32_t periodMs)
{
    if (mode == _mode && color == _color && periodMs == _period_ms) {
        return;
    }
    _mode          = mode;
    _color         = color;
    _period_ms     = std::max<uint32_t>(periodMs, 1);
    _static_dirty  = true;
    _gauge_written = -1;
}

void LedAnimator::flash(uint32_t color, uint32_t durationMs, uint32_t nowMs)
{
    _flash_color  = color;
    _flash_until  = nowMs + durationMs;
    _flashing     = true;
    _static_dirty = true;
    _last_frame   = 0;  // show the flash right away
}

void LedAnimator::setChasePosition(float turns)
{
    _chase_position = turns;
}

void LedAnimator::setGaugeLevel(float fraction)
{
    _gauge_lit = static_cast<int>(std::lround(std::clamp(fraction, 0.0f, 1.0f) * kLedCount));
}

void LedAnimator::update(uint32_t nowMs)
{
    if (_last_frame != 0 && nowMs - _last_frame < kFrameMs) {
        return;
    }
    _last_frame = nowMs == 0 ? 1 : nowMs;

    if (_flashing) {
        if (static_cast<int32_t>(nowMs - _flash_until) >= 0) {
            _flashing      = false;
            _static_dirty  = true;
            _gauge_written = -1;
        } else {
            // 8 Hz blink
            bool on = ((_flash_until - nowMs) / 62) % 2 == 0;
            if (on) {
                fill(red_of(_flash_color), green_of(_flash_color), blue_of(_flash_color));
            } else {
                off();
            }
            return;
        }
    }

    const float phase = static_cast<float>(nowMs % _period_ms) / _period_ms;
    switch (_mode) {
        case LedMode::Off:
            if (_static_dirty) {
                off();
                _static_dirty = false;
            }
            break;
        case LedMode::Solid:
            if (_static_dirty) {
                fill(red_of(_color), green_of(_color), blue_of(_color));
                _static_dirty = false;
            }
            break;
        case LedMode::Pulse:
            pulse(phase, red_of(_color), green_of(_color), blue_of(_color));
            break;
        case LedMode::Rainbow:
            rainbow(phase);
            break;
        case LedMode::Chase:
            chase(_chase_position, red_of(_color), green_of(_color), blue_of(_color));
            break;
        case LedMode::Gauge:
            if (_gauge_lit != _gauge_written) {
                gauge(static_cast<float>(std::max(_gauge_lit, 0)) / kLedCount, red_of(_color), green_of(_color),
                      blue_of(_color));
                _gauge_written = _gauge_lit;
            }
            break;
    }
}

}  // namespace stackpet::fx
