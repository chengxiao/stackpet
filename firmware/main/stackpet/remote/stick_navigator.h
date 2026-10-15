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

// The launcher with the remote (it only sends when the stick moves or a button changes):
//   Previous / Next  the stick pushed left / right: the app beside the one shown; held out there, on to the next ones
//   Open             the stick held pressed in, near the middle, for a moment: opens the app shown
enum class NavAction : uint8_t { Previous, Next, Open };

// The remote turns the stick's right into negative yaw: yaw = map(x, X_MIN, X_MAX, 1280, -1280), while the dot on its
// own screen goes right as x grows (remote/code/main/joystick/joystick_handle.c, ui/ui_running_screen.c). Right is the
// next app: towards it is yaw * kNextYaw > 0
constexpr float kNextYaw = -1.0f;

struct StickNavigatorConfig {
    float stepLevel        = 0.5f;  // pushed this far out along yaw (1 = the edge): one app that way ...
    float backLevel        = 0.3f;  // ... and back within this before the next push counts
    uint32_t repeatAfterMs = 600;   // held out there: on to the next app after this long ...
    uint32_t repeatEveryMs = 450;   // ... then this often (a step's scroll takes up to 400 ms: each one comes to rest)
    int repeatLimit        = 10;    // ... this many more at most (a lost packet must not leave it going round for good)
    uint32_t openPressMs   = 800;   // the stick held pressed in this long: open
    uint32_t heldPressMs   = 1000;  // the first packet this soon after reset(): the stick was like that before
    float pressDeflection  = kPressDeflection;  // pressed in further out: pushed too hard, no press
};

class StickNavigator {
public:
    using Config = StickNavigatorConfig;

    explicit StickNavigator(const Config& config = Config());
    // Listening starts at nowMs: everything forgotten. What the first packet after it says (pushed out, pressed in)
    // counts, unless it comes within heldPressMs: then the stick was like that before, it has to come back first.
    // quietBefore (a boot: nothing went on before): the first packet counts however soon it comes.
    // Never reset: always like that before.
    void reset(uint32_t nowMs, bool quietBefore = false);
    // Every packet meant for this robot, in order (a batch of them with the same nowMs: tick() after the batch)
    void update(uint32_t nowMs, const RemotePacket& packet);
    // Every tick, after the packets: held still the remote sends nothing, so held out goes on to the next app and held
    // in opens from here only (after a whole batch: a release in it comes first)
    void tick(uint32_t nowMs);
    std::vector<NavAction> take();
    // The stick held pressed in: how far towards opening, 0..1 (0: not pressed in, or opened already)
    float openProgress(uint32_t nowMs) const;

private:
    Config _config;
    std::vector<NavAction> _actions;
    bool _seen         = false;
    bool _since_known  = false;  // reset(nowMs) at _since_ms ...
    bool _quiet_before = false;  // ... quietBefore
    uint32_t _since_ms = 0;
    // Left / right
    int _side          = 0;      // out that way (+1 next, -1 previous) since a step, not back since
    bool _out          = false;  // ... and still pushed out there: going on now and then (tick())
    uint32_t _out_ms   = 0;      // the last step at
    int _repeats       = 0;
    // Pressed in
    bool _pressed       = false;  // held down, wherever the stick is
    bool _pressing      = false;  // ... and it counts towards opening, since _press_ms
    bool _press_steady  = false;  // ... began near the middle (to open), or was held since before
    uint32_t _press_ms  = 0;
};

}  // namespace stackpet::remote
