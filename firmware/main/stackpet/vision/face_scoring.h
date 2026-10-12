/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>

// Pure C++ helpers of FaceModel (no ESP-IDF / esp-dl dependency, host tested).
namespace stackpet::vision {

/**
 * @brief Probability of class `cls` given `classes` quantized logits (logit = value * scale): softmax over the
 * classes, a sigmoid when there is a single one.
 *
 * Stage 2 (MNP) of the face detector scores the face class with it. The largest logit is subtracted first, so any
 * int8 / int16 input stays finite (the exponents are <= 0).
 */
template <typename T>
float classProbability(const T* logits, int classes, float scale, int cls)
{
    if (classes <= 1) {
        return 1.0f / (1.0f + std::exp(-static_cast<float>(logits[0]) * scale));
    }
    float max_logit = static_cast<float>(logits[0]) * scale;
    for (int c = 1; c < classes; c++) {
        max_logit = std::max(max_logit, static_cast<float>(logits[c]) * scale);
    }
    float sum = 0.0f;
    for (int c = 0; c < classes; c++) {
        sum += std::exp(static_cast<float>(logits[c]) * scale - max_logit);
    }
    return std::exp(static_cast<float>(logits[cls]) * scale - max_logit) / sum;
}

// A candidate is kept only above the threshold (like esp-dl's postprocessors: equal is not enough)
inline bool aboveThreshold(float score, float threshold)
{
    return score > threshold;
}

// The internal heap when a FaceModel stage loads (heap_caps_* numbers)
struct InternalHeap {
    size_t freeBytes   = 0;  // MALLOC_CAP_INTERNAL (includes the reserve pool, RTC RAM and IRAM)
    size_t simdFree    = 0;  // MALLOC_CAP_INTERNAL | MALLOC_CAP_SIMD: the D/IRAM esp-dl allocates feature maps from
    size_t simdLargest = 0;  // largest block of it (the budget is one block)
};

/**
 * Reserves an internal RAM budget never takes, judged on the heap right before the stage loads: esp-dl's own small
 * objects (~20 KB of <= 512 B mallocs) come from the same D/IRAM afterwards, so less is left after the load (RADAR:
 * about 20 KB of 67 KB). DMA buffers are not at stake: the 64 KB internal reserve pool
 * (CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL) is DMA-capable but not SIMD-capable, esp-dl never allocates from it.
 */
struct InternalBudgetRules {
    size_t keepFree     = 48 * 1024;  // all internal RAM: task stacks, queues, Wi-Fi
    size_t keepSimdFree = 32 * 1024;  // the D/IRAM: small mallocs (LVGL, esp-dl's objects; they spill to PSRAM after)
    size_t blockSlack   = 4 * 1024;   // left in the block the budget comes from
    size_t minBudget    = 8 * 1024;   // a smaller budget is not worth a block: 0 (everything in PSRAM) then
};

// The internal RAM budget for a stage's feature maps: `wanted`, reduced to what the heap can spare (0 = PSRAM only)
inline size_t fitInternalBudget(size_t wanted, const InternalHeap& heap,
                                const InternalBudgetRules& rules = InternalBudgetRules())
{
    if (wanted == 0) {
        return 0;
    }
    auto room           = [](size_t have, size_t keep) { return have > keep ? have - keep : size_t{0}; };
    const size_t budget = std::min({wanted, room(heap.freeBytes, rules.keepFree),
                                    room(heap.simdFree, rules.keepSimdFree), room(heap.simdLargest, rules.blockSlack)});
    return budget >= rules.minBudget ? budget : 0;
}

}  // namespace stackpet::vision
