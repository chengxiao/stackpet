/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "remote_packet.h"
#include <cstdint>
#include <vector>

namespace stackpet::remote {

// Gestures read from the remote's packet stream (it only sends when the stick moves or a button changes):
//   Shake        the stick flicked left and right a few times quickly
//   Nod          ... up and down
//   Circle       the stick swept round its edge (direction: +1 / -1)
//   DoubleClick  the B button pressed twice quickly (it toggles the laser flag, so that flips twice)
// (The stick pressed in is the launcher's, see stick_navigator.h.)
enum class Gesture : uint8_t { Shake, Nod, Circle, DoubleClick };

struct GestureEvent {
    Gesture gesture;
    int direction = 0;  // Circle only
};

struct StickGestureConfig {
    float swingLevel        = 0.5f;    // a flick reaches this far out (1 = the edge) ...
    int swingFlips          = 3;       // ... and the side flips this often ...
    uint32_t swingWindowMs  = 1500;    // ... within this long
    float circleRadius      = 0.6f;    // sweeping round: this far out ...
    float circleLostRadius  = 0.4f;    // ... (closer to the centre, the sweep starts over)
    float circleTurnDeg     = 330.0f;  // turned this far ...
    uint32_t circleWindowMs = 2500;    // ... within this long
    uint32_t doubleClickMs  = 600;     // the two B presses this close
    uint32_t cooldownMs     = 800;     // after a stick gesture, no other stick gesture for this long
};

class StickGestures {
public:
    using Config = StickGestureConfig;

    explicit StickGestures(const Config& config = Config());
    void reset();  // listening (again): everything forgotten
    // Every packet meant for this robot, in order
    void update(uint32_t nowMs, const RemotePacket& packet);
    std::vector<GestureEvent> take();

private:
    struct Swing {
        int side         = 0;  // the side last reached: -1 / +1, 0 none yet
        int flips        = 0;
        uint32_t startMs = 0;
    };
    // value: the swinging axis, cross: the other one (a flick keeps it near the centre, a sweep round does not)
    bool track_swing(Swing& swing, float value, float cross, uint32_t nowMs);
    bool track_circle(float x, float y, uint32_t nowMs, int& direction);
    void emit(uint32_t nowMs, Gesture gesture, int direction = 0, bool stick = true);

    Config _config;
    std::vector<GestureEvent> _events;
    bool _seen          = false;
    bool _laser         = false;
    bool _toggle_armed  = false;  // one B press seen, waiting for the second
    uint32_t _toggle_ms = 0;
    uint32_t _quiet_from  = 0;  // a stick gesture's quiet time from then (cooldownMs)
    bool _cooling       = false;
    Swing _yaw_swing;
    Swing _pitch_swing;
    bool _circling       = false;
    float _circle_angle  = 0.0f;  // the last angle on the edge (degree)
    float _circle_turned = 0.0f;
    uint32_t _circle_start_ms = 0;
};

}  // namespace stackpet::remote
