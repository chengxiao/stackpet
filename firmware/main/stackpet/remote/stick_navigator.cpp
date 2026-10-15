/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "stick_navigator.h"
#include <algorithm>
#include <cmath>

namespace stackpet::remote {

namespace {

// The clock only goes forward: unsigned, so a wrap (49 days) or a long quiet spell does not matter
uint32_t elapsed(uint32_t now, uint32_t since)
{
    return now - since;
}

}  // namespace

StickNavigator::StickNavigator(const Config& config) : _config(config)
{
}

void StickNavigator::reset(uint32_t nowMs, bool quietBefore)
{
    const Config config = _config;
    *this               = StickNavigator(config);
    _since_known        = true;
    _quiet_before       = quietBefore;
    _since_ms           = nowMs;
}

void StickNavigator::update(uint32_t nowMs, const RemotePacket& packet)
{
    // The remote only sends on a change: after a quiet spell, whatever the user does is the first packet. Only right
    // after listening started is it how the stick was before
    const bool before =
        !_seen && !_quiet_before && !(_since_known && elapsed(nowMs, _since_ms) >= _config.heldPressMs);
    _seen             = true;

    // Pressed in near the middle: timed towards opening (tick())
    const bool pressed = packet.hasButtons && (packet.buttons & kButtonStick) != 0;
    const bool centred = stickDeflection(packet) <= _config.pressDeflection;
    // Let go now after a press begun out there and held a while: a long press tried off the middle, no push either
    const bool long_off = _pressed && !pressed && !_press_steady &&
                          elapsed(nowMs, _press_ms) >= _config.openPressMs / 2;
    if (!pressed) {
        _pressing = false;
    } else if (!_pressed) {
        _pressing     = !before && centred;
        _press_steady = centred || before;
        _press_ms     = nowMs;
    } else if (!centred) {
        _pressing = false;  // pushed off the middle while held: no press, until let go
    }
    _pressed = pressed;

    // Pushed left / right (not while pressed in): one step, back within backLevel before the next one
    const float x  = kNextYaw * static_cast<float>(packet.yaw) / 1280.0f;  // +: towards the next app
    const int out  = x >= _config.stepLevel ? 1 : (x <= -_config.stepLevel ? -1 : 0);
    const int side = pressed ? 0 : out;
    if (side == 0) {
        _out = false;  // eased off (or pressed in): no more going on, a new push once back
        if (std::fabs(x) < _config.backLevel) {
            _side = 0;
        } else if (pressed && out != 0 && _press_steady) {
            // A steady press (begun in the middle to open, or held since before) out there: let go out there is no
            // push. One begun out there (pressed by mistake flicking out) leaves _side as it was: a step already
            // taken stays the only one, else letting go out there is the push
            _side = out;
        }
    } else if (side != _side) {
        // Out there now, or straight over to the other side
        const bool push = !before && !long_off;
        _side           = side;
        _out            = push;
        _out_ms         = nowMs;
        _repeats        = 0;
        if (push) {
            _actions.push_back(side > 0 ? NavAction::Next : NavAction::Previous);
        }
    }
}

void StickNavigator::tick(uint32_t nowMs)
{
    const uint32_t due = _repeats == 0 ? _config.repeatAfterMs : _config.repeatEveryMs;
    if (_out && _repeats < _config.repeatLimit && elapsed(nowMs, _out_ms) >= due) {
        _actions.push_back(_side > 0 ? NavAction::Next : NavAction::Previous);
        _out_ms = nowMs;
        _repeats++;
    }
    if (_pressing && elapsed(nowMs, _press_ms) >= _config.openPressMs) {
        _pressing = false;  // once: let go and pressed in again for the next one
        _actions.push_back(NavAction::Open);
    }
}

std::vector<NavAction> StickNavigator::take()
{
    std::vector<NavAction> out;
    out.swap(_actions);
    return out;
}

float StickNavigator::openProgress(uint32_t nowMs) const
{
    if (!_pressing) {
        return 0.0f;
    }
    if (_config.openPressMs == 0) {
        return 1.0f;
    }
    return std::min(1.0f, static_cast<float>(elapsed(nowMs, _press_ms)) / static_cast<float>(_config.openPressMs));
}

}  // namespace stackpet::remote
