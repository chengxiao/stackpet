/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <stackpet/sound/tone_synth.h>

namespace {

using stackpet::Sfx;
using stackpet::Tone;
using stackpet::Wave;
using stackpet::synth::ToneRenderer;

constexpr int kRate = 24000;

void expect(bool condition, const char* label)
{
    if (!condition) {
        std::cerr << "FAILED: " << label << '\n';
        std::exit(1);
    }
}

Tone makeTone(uint16_t freq, uint16_t ms, uint8_t volume, Wave wave, uint16_t slide = 0)
{
    Tone t;
    t.freqHz     = freq;
    t.durationMs = ms;
    t.volume     = volume;
    t.wave       = wave;
    t.slideToHz  = slide;
    return t;
}

std::vector<int16_t> renderAll(const Tone& tone, size_t chunk)
{
    ToneRenderer renderer(kRate);
    renderer.begin(tone);
    std::vector<int16_t> out;
    std::vector<int16_t> buf(chunk);
    while (true) {
        size_t n = renderer.render(buf.data(), buf.size());
        if (n == 0) {
            break;
        }
        out.insert(out.end(), buf.begin(), buf.begin() + n);
    }
    expect(renderer.done(), "renderer done after rendering everything");
    return out;
}

int zeroCrossings(const std::vector<int16_t>& pcm, size_t begin, size_t end)
{
    int count = 0;
    for (size_t i = begin + 1; i < end; i++) {
        if ((pcm[i - 1] < 0 && pcm[i] >= 0) || (pcm[i - 1] >= 0 && pcm[i] < 0)) {
            count++;
        }
    }
    return count;
}

void testSampleCounts()
{
    for (Wave wave : {Wave::Square, Wave::Sine, Wave::Triangle, Wave::Noise}) {
        auto pcm = renderAll(makeTone(440, 100, 80, wave), 240);
        expect(pcm.size() == 2400, "100 ms at 24 kHz is 2400 samples");
    }
    auto odd = renderAll(makeTone(440, 37, 80, Wave::Sine), 7);
    expect(odd.size() == 37u * kRate / 1000, "odd duration and chunk size sample count");

    ToneRenderer renderer(kRate);
    renderer.begin(makeTone(440, 0, 80, Wave::Sine));
    int16_t dummy[8];
    expect(renderer.render(dummy, 8) == 0 && renderer.done(), "zero duration tone renders nothing");
}

void testNoClippingAndPeak()
{
    for (Wave wave : {Wave::Square, Wave::Sine, Wave::Triangle, Wave::Noise}) {
        int16_t peak = ToneRenderer::peakLevel(wave);
        expect(peak < 32767 * 0.7, "peak level leaves headroom");

        auto full = renderAll(makeTone(523, 200, 100, wave), 240);
        auto half = renderAll(makeTone(523, 200, 50, wave), 240);
        int max_full = 0;
        int max_half = 0;
        for (auto s : full) {
            max_full = std::max(max_full, std::abs(static_cast<int>(s)));
        }
        for (auto s : half) {
            max_half = std::max(max_half, std::abs(static_cast<int>(s)));
        }
        expect(max_full <= peak + 1, "full volume stays within the peak level");
        expect(max_full > peak / 2, "full volume is actually loud");
        expect(max_half <= peak / 2 + 1, "half volume halves the level");

        auto over = renderAll(makeTone(523, 50, 255, wave), 240);
        for (auto s : over) {
            expect(std::abs(static_cast<int>(s)) <= peak + 1, "volume above 100 is clamped");
        }
    }
}

void testEnvelopeEdges()
{
    for (Wave wave : {Wave::Square, Wave::Sine, Wave::Triangle}) {
        auto pcm = renderAll(makeTone(880, 120, 100, wave), 240);
        expect(pcm.front() == 0, "attack starts from silence");
        expect(pcm.back() == 0, "release ends in silence");
        int16_t peak = ToneRenderer::peakLevel(wave);
        for (int i = 0; i < 6; i++) {
            expect(std::abs(static_cast<int>(pcm[i])) <= peak / 10, "first samples are faded in");
            expect(std::abs(static_cast<int>(pcm[pcm.size() - 1 - i])) <= peak / 10, "last samples are faded out");
        }
    }
}

void testRest()
{
    auto pcm = renderAll(makeTone(0, 40, 100, Wave::Square), 64);
    expect(pcm.size() == 960, "rest keeps its duration");
    for (auto s : pcm) {
        expect(s == 0, "rest is silent");
    }
}

void testFrequencyAndGlide()
{
    // A 1 kHz sine for one second crosses zero about 2000 times
    auto sine = renderAll(makeTone(1000, 1000, 80, Wave::Sine), 240);
    int crossings = zeroCrossings(sine, 0, sine.size());
    expect(std::abs(crossings - 2000) <= 40, "1 kHz sine frequency");

    // Glide 400 -> 1600 Hz: the end of the tone is about 4x faster than the start
    auto glide   = renderAll(makeTone(400, 1000, 80, Wave::Sine, 1600), 240);
    size_t tenth = glide.size() / 10;
    int start    = zeroCrossings(glide, tenth / 2, tenth + tenth / 2);
    int end      = zeroCrossings(glide, glide.size() - tenth - tenth / 2, glide.size() - tenth / 2);
    float ratio  = static_cast<float>(end) / static_cast<float>(start);
    expect(ratio > 2.8f && ratio < 4.6f, "glide raises the pitch");
}

void testChunkingIsDeterministic()
{
    auto tone = makeTone(659, 300, 70, Wave::Triangle, 330);
    auto a    = renderAll(tone, 4800);
    auto b    = renderAll(tone, 13);
    expect(a == b, "chunk size does not change the output");
}

void testSfxTable()
{
    for (Sfx sfx : {Sfx::Click, Sfx::Start, Sfx::Tick, Sfx::Go, Sfx::Success, Sfx::Fail, Sfx::Caught, Sfx::Win,
                    Sfx::Reveal, Sfx::Boop, Sfx::MuTouRen}) {
        const auto& tones = stackpet::synth::sfxTones(sfx);
        expect(!tones.empty(), "every sfx has tones");
        uint32_t duration = stackpet::synth::sequenceDurationMs(tones);
        expect(duration > 0 && duration <= 1500, "every sfx is short (<= 1.5 s)");
        for (const auto& t : tones) {
            expect(t.volume <= 100, "sfx volume within 0-100");
        }
    }
    int notes = 0;
    for (const auto& t : stackpet::synth::sfxTones(Sfx::MuTouRen)) {
        if (t.freqHz > 0) {
            notes++;
        }
    }
    expect(notes == 3, "MuTouRen is a three-note chant");
    expect(stackpet::synth::sequenceDurationMs(stackpet::synth::sfxTones(Sfx::Tick)) <= 30, "tick is tiny");
}

}  // namespace

int main()
{
    testSampleCounts();
    testNoClippingAndPeak();
    testEnvelopeEdges();
    testRest();
    testFrequencyAndGlide();
    testChunkingIsDeterministic();
    testSfxTable();
    std::cout << "stackpet_audio_synth_test passed\n";
    return 0;
}
