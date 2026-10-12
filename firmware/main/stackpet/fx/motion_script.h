/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stackchan/motion/motion.h>
#include <cstdint>
#include <vector>

namespace stackpet::fx {

struct MotionKeyframe {
    uint32_t atMs = 0;  // time from play()
    int yaw       = 0;  // 0.1 degree
    int pitch     = 0;
    int speed     = 500;
};

/**
 * Tiny non-blocking keyframe player for short gestures (nod, wiggle). Each keyframe issues one
 * moveWithSpeed() when its time comes; the servo springs do the interpolation. Main loop only.
 */
class MotionScript {
public:
    void play(std::vector<MotionKeyframe> frames, uint32_t nowMs);
    void stop();
    bool isPlaying() const;
    void update(uint32_t nowMs, stackchan::motion::Motion& motion);

private:
    std::vector<MotionKeyframe> _frames;
    size_t _next       = 0;
    uint32_t _start_ms = 0;
};

}  // namespace stackpet::fx
