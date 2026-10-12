/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstdint>

namespace stackpet::fx {

/**
 * Small helpers on top of GetHAL().setRgbColor()/refreshRgb() for the 12 body LEDs
 * (index 0-5 left row, 6-11 right row). Call from the app main loop (not thread-safe).
 * Keep the update rate <= 30 Hz: every refresh is an I2C transaction to the PY32 IO expander.
 */
void fill(uint8_t r, uint8_t g, uint8_t b);
void off();
// One lit LED chasing around both rows; position in turns (fractional part used), with a fading tail.
void chase(float position, uint8_t r, uint8_t g, uint8_t b);
// Rainbow cycling with phase 0..1.
void rainbow(float phase);
// Breathing pulse of one color, phase 0..1.
void pulse(float phase, uint8_t r, uint8_t g, uint8_t b);
// Light the first round(fraction * 12) LEDs along the ring, the rest off (countdown / level meter).
void gauge(float fraction, uint8_t r, uint8_t g, uint8_t b);

enum class LedMode : uint8_t { Off, Solid, Pulse, Rainbow, Chase, Gauge };

/**
 * Time based LED animation for the app main loop: a base mode plus an optional short flash overlay.
 * update() is cheap to call every loop, it refreshes the LEDs at most kFrameMs apart and a static
 * mode (Off / Solid) is written only once.
 */
class LedAnimator {
public:
    static constexpr uint32_t kFrameMs = 40;

    // color: 0xRRGGBB; periodMs: one breath (Pulse) / one rainbow cycle (Rainbow)
    void set(LedMode mode, uint32_t color = 0, uint32_t periodMs = 1600);
    // Blink `color` on/off for durationMs on top of the base mode, then return to it
    void flash(uint32_t color, uint32_t durationMs, uint32_t nowMs);
    // Chase head position in turns (Chase mode)
    void setChasePosition(float turns);
    // Lit fraction 0..1 (Gauge mode); only refreshed when the number of lit LEDs changes
    void setGaugeLevel(float fraction);
    void update(uint32_t nowMs);

private:
    LedMode _mode          = LedMode::Off;
    uint32_t _color        = 0;
    uint32_t _period_ms    = 1600;
    float _chase_position  = 0.0f;
    int _gauge_lit         = -1;
    int _gauge_written     = -1;
    uint32_t _flash_color  = 0;
    uint32_t _flash_until  = 0;
    bool _flashing         = false;
    bool _static_dirty     = true;
    uint32_t _last_frame   = 0;
};

}  // namespace stackpet::fx
