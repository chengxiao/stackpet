/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "head_touch.h"

namespace stackpet::fx {

HeadTouch::~HeadTouch()
{
    detach();
}

void HeadTouch::attach()
{
    if (_attached) {
        return;
    }
    _connection = GetHAL().onHeadPetGesture.connect([this](HeadPetGesture gesture) {
        std::lock_guard<std::mutex> lock(_mutex);
        // A stuck main loop must not grow the queue forever
        if (_queue.size() < 32) {
            _queue.push_back(gesture);
        }
    });
    _attached = true;
}

void HeadTouch::detach()
{
    if (!_attached) {
        return;
    }
    GetHAL().onHeadPetGesture.disconnect(_connection);
    _attached = false;
    std::lock_guard<std::mutex> lock(_mutex);
    _queue.clear();
}

HeadTouchEvents HeadTouch::poll()
{
    std::vector<HeadPetGesture> gestures;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        gestures.swap(_queue);
    }

    HeadTouchEvents events;
    for (auto gesture : gestures) {
        switch (gesture) {
            case HeadPetGesture::Press:
                events.pressed = true;
                _touching      = true;
                _swiped        = false;
                break;
            case HeadPetGesture::SwipeForward:
                events.swipe++;
                _swiped = true;
                break;
            case HeadPetGesture::SwipeBackward:
                events.swipe--;
                _swiped = true;
                break;
            case HeadPetGesture::Release:
                if (_touching && !_swiped) {
                    events.tapped = true;
                }
                _touching = false;
                _swiped   = false;
                break;
            default:
                break;
        }
    }
    return events;
}

}  // namespace stackpet::fx
