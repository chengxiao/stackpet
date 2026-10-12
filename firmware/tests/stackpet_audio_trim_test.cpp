/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <stackpet/sound/pcm_trim.h>

namespace {

using stackpet::pcm::applyFadeOut;
using stackpet::pcm::dbfsToAmplitude;
using stackpet::pcm::findVoicedRange;
using stackpet::pcm::TrimConfig;
using stackpet::pcm::TrimRange;

constexpr int kRate      = 24000;
constexpr size_t kWindow = kRate * 5 / 1000;  // default analysis window, 120 samples
constexpr size_t kFade   = kRate * 30 / 1000;  // default fade-out, 720 samples

void expect(bool condition, const char* label)
{
    if (!condition) {
        std::cerr << "FAILED: " << label << '\n';
        std::exit(1);
    }
}

size_t ms(double milliseconds)
{
    return static_cast<size_t>(milliseconds * kRate / 1000.0);
}

struct Clip {
    std::vector<int16_t> pcm;
    uint32_t noise_state = 0x2468ACE1u;

    // Low level noise like the noise floor of a decoded Opus clip
    void silence(double milliseconds, float dbfs = -55.0f)
    {
        float amplitude = 32767.0f * std::pow(10.0f, dbfs / 20.0f);
        for (size_t i = 0; i < ms(milliseconds); i++) {
            noise_state ^= noise_state << 13;
            noise_state ^= noise_state >> 17;
            noise_state ^= noise_state << 5;
            float n = static_cast<float>(noise_state & 0xFFFF) / 32767.5f - 1.0f;
            pcm.push_back(static_cast<int16_t>(n * amplitude));
        }
    }

    void voice(double milliseconds, float dbfs = -6.0f)
    {
        float amplitude = 32767.0f * std::pow(10.0f, dbfs / 20.0f);
        size_t offset   = pcm.size();
        for (size_t i = 0; i < ms(milliseconds); i++) {
            pcm.push_back(static_cast<int16_t>(amplitude * std::sin(2.0 * M_PI * 440.0 * (offset + i) / kRate)));
        }
    }
};

TrimRange trim(const Clip& clip)
{
    return findVoicedRange(clip.pcm.data(), clip.pcm.size(), kRate);
}

void testThresholdConversion()
{
    expect(dbfsToAmplitude(0.0f) == 32767, "0 dBFS is full scale");
    expect(std::abs(dbfsToAmplitude(-35.0f) - 583) <= 1, "-35 dBFS is about 583");
    expect(dbfsToAmplitude(-200.0f) == 0, "very low levels round to 0");
}

void testDigitLikeClip()
{
    // Like the zh-CN digit clips: a short word followed by a long silent tail
    Clip clip;
    clip.silence(200);
    clip.voice(300);
    clip.silence(500);

    TrimRange r = trim(clip);
    expect(!r.silent, "voice is detected");
    expect(r.begin == ms(200) - kWindow, "leading silence skipped, one window of pre-roll kept");
    expect(r.end == ms(500) + kFade, "tail cut 30 ms after the voice");
    expect(r.fadeOutSamples == kFade, "30 ms fade-out");
    double played_ms = (r.end - r.begin) * 1000.0 / kRate;
    expect(played_ms > 330 && played_ms < 340, "0.97 s clip plays in about a third of the time");
}

void testShortTailIsKept()
{
    Clip clip;
    clip.voice(250);
    clip.silence(40);
    TrimRange r = trim(clip);
    expect(r.begin == 0, "no leading silence");
    expect(r.end == clip.pcm.size() && r.fadeOutSamples == 0, "a 40 ms tail is not trimmed");

    Clip longer;
    longer.voice(250);
    longer.silence(100);
    TrimRange t = trim(longer);
    expect(t.end == ms(250) + kFade && t.fadeOutSamples == kFade, "a 100 ms tail is trimmed");
}

void testGapInsideIsKept()
{
    // Two syllables with a 150 ms pause: the pause must survive, only the tail goes
    Clip clip;
    clip.silence(50);
    clip.voice(150);
    clip.silence(150);
    clip.voice(150);
    clip.silence(400);
    TrimRange r = trim(clip);
    expect(r.begin == ms(50) - kWindow, "begins at the first syllable");
    expect(r.end == ms(500) + kFade, "ends after the second syllable");
}

void testSilentAndQuietClips()
{
    Clip silent;
    silent.silence(800);
    TrimRange r = trim(silent);
    expect(r.silent && r.begin == 0 && r.end == 0, "noise floor only: nothing to play");

    Clip quiet;
    quiet.silence(100);
    quiet.voice(300, -40.0f);
    quiet.silence(300);
    expect(trim(quiet).silent, "-40 dBFS is below the -35 dBFS threshold");

    Clip soft;
    soft.silence(100);
    soft.voice(300, -30.0f);
    soft.silence(300);
    TrimRange s = trim(soft);
    expect(!s.silent && s.begin == ms(100) - kWindow, "-30 dBFS counts as voice");

    expect(findVoicedRange(nullptr, 0, kRate).silent, "empty input is silent");
}

void testTinyLoudClip()
{
    Clip clip;
    clip.voice(3);
    TrimRange r = trim(clip);
    expect(!r.silent && r.begin == 0 && r.end == clip.pcm.size(), "clip shorter than a window is kept");
}

void testCustomConfig()
{
    Clip clip;
    clip.silence(100);
    clip.voice(200);
    clip.silence(300);
    TrimConfig config;
    config.preRollMs = 0;
    config.fadeOutMs = 10;
    TrimRange r = findVoicedRange(clip.pcm.data(), clip.pcm.size(), kRate, config);
    expect(r.begin == ms(100), "no pre-roll");
    expect(r.end == ms(300) + ms(10) && r.fadeOutSamples == ms(10), "custom fade length");
}

void testFadeOut()
{
    std::vector<int16_t> pcm(1000, 10000);
    applyFadeOut(pcm.data(), pcm.size(), 100);
    for (size_t i = 0; i < 900; i++) {
        expect(pcm[i] == 10000, "samples before the fade are untouched");
    }
    expect(pcm[900] == 9900, "fade starts just below full level");
    for (size_t i = 901; i < pcm.size(); i++) {
        expect(pcm[i] <= pcm[i - 1], "fade is monotonic");
    }
    expect(pcm.back() == 0, "fade ends in silence");

    std::vector<int16_t> negative(50, -8000);
    applyFadeOut(negative.data(), negative.size(), 500);
    expect(negative.front() < 0 && negative.back() == 0, "fade longer than the buffer is clamped");

    std::vector<int16_t> untouched(10, 123);
    applyFadeOut(untouched.data(), untouched.size(), 0);
    expect(untouched.back() == 123, "zero length fade does nothing");
}

}  // namespace

int main()
{
    testThresholdConversion();
    testDigitLikeClip();
    testShortTailIsKept();
    testGapInsideIsKept();
    testSilentAndQuietClips();
    testTinyLoudClip();
    testCustomConfig();
    testFadeOut();
    std::cout << "stackpet_audio_trim_test passed\n";
    return 0;
}
