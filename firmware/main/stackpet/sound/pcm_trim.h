/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstddef>
#include <cstdint>

namespace stackpet::pcm {

/**
 * Silence trimming for short voice clips (pure C++, host testable).
 *
 * The embedded zh-CN digit clips are ~0.97 s long but only ~0.3 s of that is voice, which makes a chant
 * like "一、二、三" drag. Leading silence is dropped, and a trailing silence longer than minTrailingSilenceMs
 * is cut right after the last loud part, keeping a short fade-out so the cut never clicks.
 * Silence inside the clip (e.g. between syllables) is kept.
 */
struct TrimConfig {
    float thresholdDbfs           = -35.0f;  // a window whose peak is below this level counts as silence
    uint32_t windowMs             = 5;       // analysis window
    uint32_t preRollMs            = 5;       // audio kept before the first loud window (keeps the attack)
    uint32_t minTrailingSilenceMs = 60;      // the tail is only trimmed if it is silent for longer than this
    uint32_t fadeOutMs            = 30;      // fade-out kept after the last loud window when the tail is trimmed
};

struct TrimRange {
    size_t begin          = 0;      // first sample to play
    size_t end            = 0;      // one past the last sample to play
    size_t fadeOutSamples = 0;      // fade the last fadeOutSamples of [begin, end) to zero (0 = no fade)
    bool silent           = false;  // the whole clip is below the threshold: play nothing
};

// Peak amplitude (int16 scale) that corresponds to a dBFS level.
int16_t dbfsToAmplitude(float dbfs);

TrimRange findVoicedRange(const int16_t* pcm, size_t count, int sampleRate, const TrimConfig& config = {});

// Linear fade of the last fadeSamples of pcm[0, count) down to zero.
void applyFadeOut(int16_t* pcm, size_t count, size_t fadeSamples);

}  // namespace stackpet::pcm
