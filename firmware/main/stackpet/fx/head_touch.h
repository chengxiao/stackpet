/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <hal/hal.h>
#include <cstddef>
#include <mutex>
#include <vector>

namespace stackpet::fx {

struct HeadTouchEvents {
    bool pressed = false;  // a touch started (fires before we know whether it is a tap or a swipe)
    bool tapped  = false;  // a touch ended without any swipe
    int swipe    = 0;      // net swipes: +1 per SwipeForward, -1 per SwipeBackward
};

/**
 * Collects GetHAL().onHeadPetGesture (emitted from the head touch task) and turns it into per-loop
 * events for the app main loop. The gesture stream is Press -> [Swipe...] -> Release.
 */
class HeadTouch {
public:
    ~HeadTouch();

    void attach();  // idempotent
    void detach();  // idempotent
    HeadTouchEvents poll();  // call from the main loop; consumes the gestures received since the last call

private:
    std::mutex _mutex;
    std::vector<HeadPetGesture> _queue;
    size_t _connection = 0;
    bool _attached     = false;
    bool _touching     = false;
    bool _swiped       = false;
};

}  // namespace stackpet::fx
