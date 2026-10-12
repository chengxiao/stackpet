/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "motion_script.h"
#include <algorithm>

namespace stackpet::fx {

void MotionScript::play(std::vector<MotionKeyframe> frames, uint32_t nowMs)
{
    std::stable_sort(frames.begin(), frames.end(),
                     [](const MotionKeyframe& a, const MotionKeyframe& b) { return a.atMs < b.atMs; });
    _frames   = std::move(frames);
    _next     = 0;
    _start_ms = nowMs;
}

void MotionScript::stop()
{
    _frames.clear();
    _next = 0;
}

bool MotionScript::isPlaying() const
{
    return _next < _frames.size();
}

void MotionScript::update(uint32_t nowMs, stackchan::motion::Motion& motion)
{
    while (_next < _frames.size() && nowMs - _start_ms >= _frames[_next].atMs) {
        const auto& frame = _frames[_next++];
        motion.moveWithSpeed(frame.yaw, frame.pitch, frame.speed);
    }
}

}  // namespace stackpet::fx
