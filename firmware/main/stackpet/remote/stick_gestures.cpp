/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "stick_gestures.h"
#include <cmath>

namespace stackpet::remote {

namespace {

// The clock only goes forward: unsigned, so a wrap (49 days) or a long quiet spell does not matter
uint32_t elapsed(uint32_t now, uint32_t since)
{
    return now - since;
}

constexpr float kRadToDeg   = 57.2957795f;
constexpr float kCrossLimit = 0.5f;  // a flick along one axis keeps the other one near the centre (a sweep does not)

}  // namespace

StickGestures::StickGestures(const Config& config) : _config(config)
{
}

void StickGestures::reset()
{
    const Config config = _config;
    *this               = StickGestures(config);
}

void StickGestures::update(uint32_t nowMs, const RemotePacket& packet)
{
    if (!_seen) {
        // The laser flag in the first packet is just how it is
        _seen  = true;
        _laser = packet.laser;
    } else if (packet.laser != _laser) {
        // A B press flips the laser flag: two flips close together are a double click
        _laser = packet.laser;
        if (_toggle_armed && elapsed(nowMs, _toggle_ms) <= _config.doubleClickMs) {
            _toggle_armed = false;
            emit(nowMs, Gesture::DoubleClick, 0, false);
        } else {
            _toggle_armed = true;
            _toggle_ms    = nowMs;
        }
    }

    const float x = static_cast<float>(packet.yaw) / 1280.0f;
    const float y = (static_cast<float>(packet.pitch) - 450.0f) / 450.0f;
    if (_cooling) {
        if (elapsed(nowMs, _quiet_from) < _config.cooldownMs) {
            return;
        }
        _cooling = false;
    }
    if (track_swing(_yaw_swing, x, y, nowMs)) {
        emit(nowMs, Gesture::Shake);
        return;
    }
    if (track_swing(_pitch_swing, y, x, nowMs)) {
        emit(nowMs, Gesture::Nod);
        return;
    }
    int direction = 0;
    if (track_circle(x, y, nowMs, direction)) {
        emit(nowMs, Gesture::Circle, direction);
    }
}

std::vector<GestureEvent> StickGestures::take()
{
    std::vector<GestureEvent> out;
    out.swap(_events);
    return out;
}

bool StickGestures::track_swing(Swing& swing, float value, float cross, uint32_t nowMs)
{
    int side = 0;
    if (std::fabs(cross) < kCrossLimit) {
        side = value >= _config.swingLevel ? 1 : (value <= -_config.swingLevel ? -1 : 0);
    }
    if (side == 0) {
        return false;
    }
    if (swing.side == 0 || elapsed(nowMs, swing.startMs) > _config.swingWindowMs) {
        // A new run of flicks (or the last one was too slow)
        swing.side    = side;
        swing.flips   = 0;
        swing.startMs = nowMs;
        return false;
    }
    if (side != swing.side) {
        swing.side = side;
        if (++swing.flips >= _config.swingFlips) {
            swing = Swing{};
            return true;
        }
    }
    return false;
}

bool StickGestures::track_circle(float x, float y, uint32_t nowMs, int& direction)
{
    const float r = std::sqrt(x * x + y * y);
    if (r < _config.circleLostRadius) {
        _circling = false;
        return false;
    }
    const float angle = std::atan2(y, x) * kRadToDeg;
    if (!_circling || elapsed(nowMs, _circle_start_ms) > _config.circleWindowMs) {
        _circling = r >= _config.circleRadius;
        _circle_angle    = angle;
        _circle_turned   = 0.0f;
        _circle_start_ms = nowMs;
        return false;
    }
    float d = angle - _circle_angle;
    while (d > 180.0f) {
        d -= 360.0f;
    }
    while (d <= -180.0f) {
        d += 360.0f;
    }
    _circle_angle = angle;
    _circle_turned += d;
    if (std::fabs(_circle_turned) >= _config.circleTurnDeg) {
        direction = _circle_turned > 0.0f ? 1 : -1;
        _circling = false;
        return true;
    }
    return false;
}

void StickGestures::emit(uint32_t nowMs, Gesture gesture, int direction, bool stick)
{
    _events.push_back(GestureEvent{gesture, direction});
    if (stick) {
        // One gesture at a time: the stick has to settle before the next one counts
        _cooling     = true;
        _quiet_from  = nowMs;
        _yaw_swing   = Swing{};
        _pitch_swing = Swing{};
        _circling    = false;
    }
}

}  // namespace stackpet::remote
