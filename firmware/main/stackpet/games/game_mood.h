/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "freeze_game.h"
#include "spin_game.h"
#include <stackchan/avatar/avatar/elements/emotion.h>
#include <cstdint>
#include <vector>

namespace stackpet::games {

/**
 * The robot's facial "acting" for FREEZE and SPIN. Pure C++, time is injected, host testable.
 *
 * Every game phase maps to a short emotion script (beats), e.g. "caught you": Surprised -> Angry -> Laughing.
 * The face skins blink (~120 ms) to hide every shape change, so a beat is never shorter than kMinBeatMs.
 */
using Emotion = stackchan::avatar::Emotion;

constexpr uint32_t kMinBeatMs = 400;

struct MoodBeat {
    Emotion emotion     = Emotion::Neutral;
    uint32_t durationMs = 0;  // 0 = hold until the next script (only valid for the last beat)
};

/**
 * Plays one emotion script at a time; play() replaces the current script.
 */
class MoodScript {
public:
    // An empty script is ignored: the current one keeps going
    void play(std::vector<MoodBeat> beats, uint32_t nowMs);
    // The emotion to show now. Returns true only when it differs from the previously reported one,
    // so the app calls setEmotion() (and the skins blink) only on a real change.
    bool update(uint32_t nowMs, Emotion& out);
    bool active() const;

private:
    std::vector<MoodBeat> _beats;
    uint32_t _start_ms   = 0;
    Emotion _reported    = Emotion::Neutral;
    bool _has_reported   = false;
};

/* ---------------------------------- FREEZE --------------------------------- */
// Script played when a phase is entered (never empty)
std::vector<MoodBeat> freezeMood(FreezePhase phase);

/**
 * While watching: the calm suspicious stare (Doubt) turns into wide eyes (Surprised) when somebody almost
 * moved too much, so the players see the robot "noticed something". Hysteresis plus a minimum dwell time
 * keep the face from flickering on a noisy motion level.
 */
class WatchMood {
public:
    static constexpr float kAlertOn     = 0.6f;   // danger (motion / threshold) that widens the eyes
    static constexpr float kAlertOff    = 0.35f;  // danger below this calms down again
    static constexpr uint32_t kDwellMs  = 500;    // minimum time between two changes

    void reset(uint32_t nowMs);  // back to Doubt
    Emotion update(float danger, uint32_t nowMs);
    Emotion current() const;

private:
    Emotion _current       = Emotion::Doubt;
    uint32_t _changed_ms   = 0;
};

/* ----------------------------------- SPIN ---------------------------------- */
// Script for entering a phase; empty = keep the current script (Cruise, Settle, Reveal)
std::vector<MoodBeat> spinMood(SpinPhase phase);
// Reaction to a revealed result
std::vector<MoodBeat> spinRevealMood(SpinTone tone);

}  // namespace stackpet::games
