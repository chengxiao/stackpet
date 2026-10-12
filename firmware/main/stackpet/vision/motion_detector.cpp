/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "motion_detector.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace stackpet::vision {

// Exposure compensation: auto exposure mostly scales brightness, so the reference is scaled by the
// ratio of the frame means (clamped). In very dark scenes the ratio is unstable, fall back to an offset.
static constexpr float kMinGain             = 0.6f;
static constexpr float kMaxGain             = 1.6f;
static constexpr float kMinMeanForGain      = 12.0f;

void MotionDetector::setCellThreshold(uint8_t threshold)
{
    _threshold = threshold;
}

uint8_t MotionDetector::cellThreshold() const
{
    return _threshold;
}

void MotionDetector::setReference(const uint8_t* luma, int width, int height)
{
    if (luma == nullptr || width <= 0 || height <= 0) {
        clearReference();
        return;
    }
    _reference.assign(luma, luma + static_cast<size_t>(width) * height);
    _width  = width;
    _height = height;
}

void MotionDetector::clearReference()
{
    _reference.clear();
    _width  = 0;
    _height = 0;
}

bool MotionDetector::hasReference() const
{
    return !_reference.empty();
}

MotionResult MotionDetector::compare(const uint8_t* luma, int width, int height) const
{
    MotionResult result;
    if (!hasReference() || luma == nullptr || width != _width || height != _height) {
        return result;
    }

    const int count = width * height;

    // Global brightness of both frames
    int64_t sum_ref = 0;
    int64_t sum_cur = 0;
    for (int i = 0; i < count; i++) {
        sum_ref += _reference[i];
        sum_cur += luma[i];
    }
    const float mean_ref = static_cast<float>(sum_ref) / count;
    const float mean_cur = static_cast<float>(sum_cur) / count;
    result.meanShift     = mean_cur - mean_ref;

    const bool use_gain = mean_ref >= kMinMeanForGain;
    const float gain    = use_gain ? std::clamp(mean_cur / mean_ref, kMinGain, kMaxGain) : 1.0f;
    // Whatever the gain clamp did not explain is removed as an offset, so the compensated means match
    const float offset = mean_cur - mean_ref * gain;

    // Pass 1: raw changed mask
    auto& changed = _changed;
    changed.assign(count, 0);
    for (int i = 0; i < count; i++) {
        const float predicted = _reference[i] * gain + offset;
        const float diff      = std::fabs(static_cast<float>(luma[i]) - predicted);
        if (diff > _threshold) {
            changed[i] = static_cast<uint8_t>(std::min(255.0f, diff - _threshold + 1.0f));
        }
    }

    // Pass 2: drop isolated cells (sensor noise); a real moving object covers neighbouring cells
    auto& kept = _kept;
    kept.assign(count, 0);
    float weight_sum = 0.0f;
    float weighted_x = 0.0f;
    float weighted_y = 0.0f;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const int i = y * width + x;
            if (!changed[i]) {
                continue;
            }
            const bool has_neighbour = (x > 0 && changed[i - 1]) || (x + 1 < width && changed[i + 1]) ||
                                       (y > 0 && changed[i - width]) || (y + 1 < height && changed[i + width]);
            if (!has_neighbour) {
                continue;
            }
            kept[i] = 1;
            result.changedCells++;
            const float w = changed[i];
            weight_sum += w;
            weighted_x += w * x;
            weighted_y += w * y;
        }
    }

    auto to_normalized_x = [width](float x) { return width > 1 ? x / (width - 1) * 2.0f - 1.0f : 0.0f; };
    auto to_normalized_y = [height](float y) { return height > 1 ? y / (height - 1) * 2.0f - 1.0f : 0.0f; };

    result.valid = true;
    result.score = static_cast<float>(result.changedCells) / count;
    if (weight_sum > 0.0f) {
        result.cx = to_normalized_x(weighted_x / weight_sum);
        result.cy = to_normalized_y(weighted_y / weight_sum);
    }

    // Pass 3: largest 4-connected blob (flood fill, kept cells are cleared as they are visited)
    auto& stack = _stack;
    for (int start = 0; start < count; start++) {
        if (!kept[start]) {
            continue;
        }
        int cells     = 0;
        float blob_wx = 0.0f;
        float blob_wy = 0.0f;
        float blob_w  = 0.0f;
        stack.clear();
        stack.push_back(start);
        kept[start] = 0;
        while (!stack.empty()) {
            const int i = stack.back();
            stack.pop_back();
            const int x   = i % width;
            const int y   = i / width;
            const float w = changed[i];
            cells++;
            blob_w += w;
            blob_wx += w * x;
            blob_wy += w * y;
            if (x > 0 && kept[i - 1]) {
                kept[i - 1] = 0;
                stack.push_back(i - 1);
            }
            if (x + 1 < width && kept[i + 1]) {
                kept[i + 1] = 0;
                stack.push_back(i + 1);
            }
            if (y > 0 && kept[i - width]) {
                kept[i - width] = 0;
                stack.push_back(i - width);
            }
            if (y + 1 < height && kept[i + width]) {
                kept[i + width] = 0;
                stack.push_back(i + width);
            }
        }
        if (cells > result.blobCells && blob_w > 0.0f) {
            result.blobCells = cells;
            result.blobCx    = to_normalized_x(blob_wx / blob_w);
            result.blobCy    = to_normalized_y(blob_wy / blob_w);
        }
    }
    return result;
}

float meanLuma(const uint8_t* luma, int width, int height)
{
    if (luma == nullptr || width <= 0 || height <= 0) {
        return 0.0f;
    }
    const int count = width * height;
    int64_t sum     = 0;
    for (int i = 0; i < count; i++) {
        sum += luma[i];
    }
    return static_cast<float>(sum) / count;
}

}  // namespace stackpet::vision
