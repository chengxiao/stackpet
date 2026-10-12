/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "pcm_trim.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace stackpet::pcm {

int16_t dbfsToAmplitude(float dbfs)
{
    float amplitude = 32767.0f * std::pow(10.0f, dbfs / 20.0f);
    return static_cast<int16_t>(std::clamp<long>(std::lround(amplitude), 0, 32767));
}

TrimRange findVoicedRange(const int16_t* pcm, size_t count, int sampleRate, const TrimConfig& config)
{
    TrimRange range;
    if (pcm == nullptr || count == 0 || sampleRate <= 0) {
        range.silent = true;
        return range;
    }

    const size_t rate      = static_cast<size_t>(sampleRate);
    const size_t window    = std::max<size_t>(1, rate * config.windowMs / 1000);
    const int threshold    = std::max<int>(1, dbfsToAmplitude(config.thresholdDbfs));
    bool found             = false;
    size_t first_loud      = 0;
    size_t last_loud_end   = 0;

    for (size_t start = 0; start < count; start += window) {
        size_t stop = std::min(count, start + window);
        int peak    = 0;
        for (size_t i = start; i < stop; i++) {
            peak = std::max(peak, std::abs(static_cast<int>(pcm[i])));
        }
        if (peak >= threshold) {
            if (!found) {
                first_loud = start;
                found      = true;
            }
            last_loud_end = stop;
        }
    }

    if (!found) {
        range.silent = true;
        return range;
    }

    const size_t pre_roll = rate * config.preRollMs / 1000;
    range.begin           = first_loud > pre_roll ? first_loud - pre_roll : 0;

    const size_t trailing     = count - last_loud_end;
    const size_t min_trailing = rate * config.minTrailingSilenceMs / 1000;
    if (trailing > min_trailing) {
        const size_t fade    = rate * config.fadeOutMs / 1000;
        range.end            = std::min(count, last_loud_end + fade);
        range.fadeOutSamples = range.end - last_loud_end;
    } else {
        range.end            = count;
        range.fadeOutSamples = 0;
    }
    return range;
}

void applyFadeOut(int16_t* pcm, size_t count, size_t fadeSamples)
{
    if (pcm == nullptr || count == 0 || fadeSamples == 0) {
        return;
    }
    fadeSamples       = std::min(fadeSamples, count);
    const size_t base = count - fadeSamples;
    for (size_t i = 0; i < fadeSamples; i++) {
        // Gain goes from just below 1 down to exactly 0 on the last sample
        float gain       = static_cast<float>(fadeSamples - 1 - i) / static_cast<float>(fadeSamples);
        pcm[base + i] = static_cast<int16_t>(std::lround(pcm[base + i] * gain));
    }
}

}  // namespace stackpet::pcm
