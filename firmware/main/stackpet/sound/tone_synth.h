/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "sound_player.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace stackpet::synth {

/**
 * Pure C++ tone renderer (no ESP-IDF deps, host testable).
 *
 * Renders one Tone as 16-bit mono PCM in chunks. Every tone gets a short attack/release envelope so
 * consecutive tones never click, and the peak level is bounded below full scale so it never clips.
 */
class ToneRenderer {
public:
    explicit ToneRenderer(int sampleRate);

    void begin(const Tone& tone);
    // Render up to maxSamples; returns the number written (0 once the tone is finished).
    size_t render(int16_t* out, size_t maxSamples);
    bool done() const;
    size_t totalSamples() const;
    int sampleRate() const;

    // Highest absolute sample value a tone with volume 100 can produce for the given wave.
    static int16_t peakLevel(Wave wave);

private:
    float envelope(size_t index) const;
    float oscillator(float phase);

    int _sample_rate        = 24000;
    Tone _tone;
    size_t _total           = 0;
    size_t _position        = 0;
    size_t _attack          = 0;
    size_t _release         = 0;
    float _phase            = 0.0f;
    float _amplitude        = 0.0f;
    uint32_t _noise_state   = 0x12345678;
    float _noise_hold       = 0.0f;
    float _noise_phase      = 0.0f;
};

// Tone sequence of a built-in sound effect.
const std::vector<Tone>& sfxTones(Sfx sfx);

// Total duration of a tone sequence in milliseconds.
uint32_t sequenceDurationMs(const std::vector<Tone>& tones);

}  // namespace stackpet::synth
