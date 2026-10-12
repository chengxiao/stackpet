/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "tone_synth.h"
#include <algorithm>
#include <cmath>

namespace stackpet::synth {

namespace {

constexpr float kTwoPi = 6.28318530718f;

// Peak level per waveform as a fraction of full scale. Square/noise are perceptually louder, so they get
// less headroom-hungry levels; all of them stay well below 1.0, so a tone can never clip.
constexpr float kPeakSine     = 0.55f;
constexpr float kPeakTriangle = 0.60f;
constexpr float kPeakSquare   = 0.30f;
constexpr float kPeakNoise    = 0.22f;

constexpr uint32_t kAttackMs  = 5;
constexpr uint32_t kReleaseMs = 20;

float peak_fraction(Wave wave)
{
    switch (wave) {
        case Wave::Sine:
            return kPeakSine;
        case Wave::Triangle:
            return kPeakTriangle;
        case Wave::Noise:
            return kPeakNoise;
        case Wave::Square:
        default:
            return kPeakSquare;
    }
}

Tone tone(uint16_t freqHz, uint16_t durationMs, uint8_t volume, Wave wave, uint16_t slideToHz = 0)
{
    Tone t;
    t.freqHz     = freqHz;
    t.durationMs = durationMs;
    t.volume     = volume;
    t.wave       = wave;
    t.slideToHz  = slideToHz;
    return t;
}

Tone rest(uint16_t durationMs)
{
    return tone(0, durationMs, 0, Wave::Square);
}

}  // namespace

ToneRenderer::ToneRenderer(int sampleRate) : _sample_rate(sampleRate > 0 ? sampleRate : 24000)
{
}

void ToneRenderer::begin(const Tone& tone)
{
    _tone     = tone;
    _total    = static_cast<size_t>(tone.durationMs) * static_cast<size_t>(_sample_rate) / 1000;
    _position = 0;
    _phase    = 0.0f;

    // Short tones get a proportionally shorter envelope so they still reach their level
    _attack  = std::min<size_t>(static_cast<size_t>(kAttackMs) * _sample_rate / 1000, _total / 4);
    _release = std::min<size_t>(static_cast<size_t>(kReleaseMs) * _sample_rate / 1000, _total * 2 / 5);

    float volume = std::min<uint8_t>(tone.volume, 100) / 100.0f;
    _amplitude   = volume * peak_fraction(tone.wave) * 32767.0f;

    _noise_phase = 1.0f;  // force a fresh noise value on the first sample
}

bool ToneRenderer::done() const
{
    return _position >= _total;
}

size_t ToneRenderer::totalSamples() const
{
    return _total;
}

int ToneRenderer::sampleRate() const
{
    return _sample_rate;
}

int16_t ToneRenderer::peakLevel(Wave wave)
{
    return static_cast<int16_t>(peak_fraction(wave) * 32767.0f);
}

float ToneRenderer::envelope(size_t index) const
{
    if (_attack > 0 && index < _attack) {
        return static_cast<float>(index) / static_cast<float>(_attack);
    }
    size_t remaining = _total - index;  // >= 1
    if (_release > 0 && remaining <= _release) {
        return static_cast<float>(remaining - 1) / static_cast<float>(_release);
    }
    return 1.0f;
}

float ToneRenderer::oscillator(float phase)
{
    switch (_tone.wave) {
        case Wave::Sine:
            return std::sin(kTwoPi * phase);
        case Wave::Triangle:
            return 1.0f - 4.0f * std::fabs(phase - 0.5f);
        case Wave::Noise:
            return _noise_hold;
        case Wave::Square:
        default:
            return phase < 0.5f ? 1.0f : -1.0f;
    }
}

size_t ToneRenderer::render(int16_t* out, size_t maxSamples)
{
    if (out == nullptr || done()) {
        return 0;
    }

    size_t count     = std::min(maxSamples, _total - _position);
    const bool silent = _tone.freqHz == 0 && _tone.slideToHz == 0;
    const float f0    = static_cast<float>(_tone.freqHz);
    const float f1    = _tone.slideToHz > 0 ? static_cast<float>(_tone.slideToHz) : f0;
    const float inv_total = 1.0f / static_cast<float>(_total);
    const float inv_rate  = 1.0f / static_cast<float>(_sample_rate);

    for (size_t i = 0; i < count; i++) {
        size_t index = _position + i;
        if (silent) {
            out[i] = 0;
            continue;
        }

        float freq = f0 + (f1 - f0) * (static_cast<float>(index) * inv_total);

        if (_tone.wave == Wave::Noise) {
            // Sample-and-hold noise: the hold rate follows the tone frequency, which gives the noise a pitch
            float hold_rate = freq > 0.0f ? freq * 2.0f * inv_rate : 1.0f;
            _noise_phase += hold_rate;
            if (_noise_phase >= 1.0f) {
                _noise_phase -= std::floor(_noise_phase);
                // xorshift32
                _noise_state ^= _noise_state << 13;
                _noise_state ^= _noise_state >> 17;
                _noise_state ^= _noise_state << 5;
                _noise_hold = static_cast<float>(_noise_state & 0xFFFF) / 32767.5f - 1.0f;
            }
        }

        float value = oscillator(_phase) * _amplitude * envelope(index);
        _phase += freq * inv_rate;
        _phase -= std::floor(_phase);

        long sample = std::lround(value);
        out[i]      = static_cast<int16_t>(std::clamp<long>(sample, -32767, 32767));
    }

    _position += count;
    return count;
}

const std::vector<Tone>& sfxTones(Sfx sfx)
{
    // Note frequencies: C5 523, E5 659, G5 784, A5 880, C6 1047, E6 1319
    static const std::vector<Tone> click = {tone(1600, 28, 45, Wave::Square, 1100)};
    static const std::vector<Tone> start = {
        tone(523, 70, 55, Wave::Square),
        tone(659, 70, 55, Wave::Square),
        tone(784, 70, 55, Wave::Square),
        tone(1047, 170, 55, Wave::Square),
    };
    static const std::vector<Tone> tick = {tone(2200, 14, 40, Wave::Square)};
    static const std::vector<Tone> go   = {
        tone(330, 140, 65, Wave::Square, 660),
        tone(880, 230, 65, Wave::Square),
    };
    static const std::vector<Tone> success = {
        tone(659, 90, 70, Wave::Triangle),
        tone(880, 90, 70, Wave::Triangle),
        tone(1319, 200, 70, Wave::Triangle),
    };
    // Sad trombone: "wah wah wah wahhh"
    static const std::vector<Tone> fail = {
        tone(466, 280, 45, Wave::Square, 440),
        tone(440, 280, 45, Wave::Square, 415),
        tone(415, 280, 45, Wave::Square, 392),
        tone(392, 560, 45, Wave::Square, 330),
    };
    // Two-tone alarm ending on a high "gotcha"
    static const std::vector<Tone> caught = {
        tone(988, 110, 60, Wave::Square),
        tone(740, 110, 60, Wave::Square),
        tone(988, 110, 60, Wave::Square),
        tone(740, 110, 60, Wave::Square),
        tone(1319, 260, 60, Wave::Square),
    };
    // Victory fanfare
    static const std::vector<Tone> win = {
        tone(523, 90, 55, Wave::Square),
        tone(659, 90, 55, Wave::Square),
        tone(784, 90, 55, Wave::Square),
        tone(1047, 220, 55, Wave::Square),
        rest(40),
        tone(784, 110, 55, Wave::Square),
        tone(1047, 380, 55, Wave::Square),
    };
    // Rising sparkle
    static const std::vector<Tone> reveal = {
        tone(1568, 50, 50, Wave::Sine),
        tone(1976, 50, 50, Wave::Sine),
        tone(2349, 50, 50, Wave::Sine),
        tone(3136, 50, 50, Wave::Sine),
        tone(2637, 200, 50, Wave::Sine),
    };
    // Cute robot "boop"
    static const std::vector<Tone> boop = {
        tone(700, 110, 70, Wave::Sine, 350),
        tone(350, 90, 70, Wave::Sine, 600),
    };
    // "木-头-人": sol sol mi, the last note held and sagging a little
    static const std::vector<Tone> mutouren = {
        tone(784, 210, 60, Wave::Square),
        rest(40),
        tone(784, 210, 60, Wave::Square),
        rest(40),
        tone(659, 520, 60, Wave::Square, 622),
    };

    // Sonar ping: a short sine with a slight downward glide, then two fading echoes
    static const std::vector<Tone> radar_ping = {
        tone(1400, 90, 85, Wave::Sine, 1360),
        rest(120),
        tone(1400, 70, 32, Wave::Sine, 1360),
        rest(150),
        tone(1400, 60, 12, Wave::Sine, 1360),
    };
    // Two-tone siren "nee-naw" (C6 / G5), 6 cycles = 3 s
    static const std::vector<Tone> alarm = [] {
        std::vector<Tone> tones;
        for (int i = 0; i < 6; i++) {
            tones.push_back(tone(1047, 250, 80, Wave::Square));
            tones.push_back(tone(784, 250, 80, Wave::Square));
        }
        return tones;
    }();

    switch (sfx) {
        case Sfx::Click:
            return click;
        case Sfx::Start:
            return start;
        case Sfx::Tick:
            return tick;
        case Sfx::Go:
            return go;
        case Sfx::Success:
            return success;
        case Sfx::Fail:
            return fail;
        case Sfx::Caught:
            return caught;
        case Sfx::Win:
            return win;
        case Sfx::Reveal:
            return reveal;
        case Sfx::Boop:
            return boop;
        case Sfx::MuTouRen:
            return mutouren;
        case Sfx::RadarPing:
            return radar_ping;
        case Sfx::Alarm:
            return alarm;
    }
    return click;
}

uint32_t sequenceDurationMs(const std::vector<Tone>& tones)
{
    uint32_t total = 0;
    for (const auto& t : tones) {
        total += t.durationMs;
    }
    return total;
}

}  // namespace stackpet::synth
