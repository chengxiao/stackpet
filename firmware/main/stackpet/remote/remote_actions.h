/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "stick_gestures.h"
#include <apps/app_radar/radar_logic.h>
#include <stackchan/avatar/avatar/elements/emotion.h>
#include <cstdint>

namespace stackpet::remote {

/**
 * What the robot does for a stick gesture in the remote control app:
 *   Shake        shakes its head, "不要～"
 *   Nod          nods, "好的！"
 *   Circle       spins round once the way the stick went (wheel mode), then is dizzy for a moment
 *   DoubleClick  puts on the next face (it stays, also after the other acts)
 * When an act ends the head goes where the stick is by then. Main loop, with the LVGL lock held (it sets the avatar).
 */
class RemoteActions {
public:
    // Starts the gesture's act; yaw / pitch: where the stick is now
    void start(const GestureEvent& event, uint32_t nowMs, int yaw, int pitch);
    // Says words with that face for a moment, then the face is back as it was
    void announce(uint32_t nowMs, const char* words, stackchan::avatar::Emotion emotion);
    // Every tick; stickYaw / stickPitch: where the stick is now (where the head goes when an act ends)
    void update(uint32_t nowMs, int stickYaw, int stickPitch);
    bool busy() const;  // the head is the act's (the stick does not move it meanwhile)
    // Closing: an act under way ends now; a spin coasts down and leaves the sensor's dead band first (blocking for
    // a moment at most), then the yaw servo is back in position mode
    void stop();

private:
    enum class Act { None, Shake, Nod, Spin };
    enum class SpinStage { WaitRest, Turning, Coasting };
    void leave_spin();  // blocking: coast down, off the dead band, position mode

    void begin_act(Act act, uint32_t nowMs, int yaw, int pitch);
    void end_act(uint32_t nowMs);
    void update_keyframes(uint32_t nowMs);
    void update_spin(uint32_t nowMs);

    Act _act               = Act::None;
    uint32_t _act_ms       = 0;
    int _keyframe          = 0;
    uint32_t _mood_from    = 0;  // the act's face and words from then ...
    uint32_t _mood_len     = 0;  // ... this long
    bool _mood_on          = false;
    int _face_index        = 0;
    stackchan::avatar::Emotion _face = stackchan::avatar::Emotion::Neutral;  // between acts (DoubleClick's pick)
    int _stick_yaw         = 0;
    int _stick_pitch       = 450;
    // Spin
    SpinStage _spin_stage  = SpinStage::WaitRest;
    int _spin_direction    = 1;  // +1: towards +yaw
    int _spin_start_yaw    = 0;
    uint32_t _stage_ms     = 0;
    uint32_t _poll_ms      = 0;
    bool _spinning         = false;
    stackpet::radar::SpinTracker _tracker;
};

}  // namespace stackpet::remote
