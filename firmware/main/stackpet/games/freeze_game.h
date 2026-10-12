/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "../vision/motion_detector.h"
#include <cstdint>
#include <vector>

namespace stackpet::games {

/**
 * "一二三木头人" (Red light, green light) game logic. Pure C++, time is injected, host testable.
 *
 * The robot turns away and chants "一、二、三、木头人", then snaps back and watches the player with the
 * camera. Any motion while it is watching => caught. Touching the robot's head before being caught => win.
 */
enum class FreezePhase : uint8_t {
    Idle = 0,     // waiting for the player to start
    Ready,        // short intro countdown, robot faces the player
    Chanting,     // robot turned away, saying "一、二、三、木头人"
    TurningBack,  // robot is turning back to face the player
    Settling,     // wait for servo vibration and camera auto exposure to settle
    Watching,     // robot stares; motion above threshold => Caught
    Survived,     // player stayed still: short celebration, then the next (harder) round
    Caught,       // a player moved: "caught you!" presentation
    PlayerWin,    // player touched the head before being caught
    CaughtWait,   // after Caught (continueAfterCaught): the caught player is out, continueGame() or start()
};

struct FreezeConfig {
    uint32_t seed              = 1;
    uint32_t readyMs           = 5000;   // time to walk back to the start line
    // Gap between chant words, counted from the moment the previous word finished playing (see audioBusy)
    uint32_t chantGapMinMs     = 600;    // random gap in round 0
    uint32_t chantGapMaxMs     = 1300;
    float chantSpeedup         = 0.85f;  // gap multiplier per survived round
    uint32_t chantGapFloorMs   = 350;
    uint32_t finalWordTurnDelayMs   = 500;   // from the "木头人" word to turning back = start of its last note "人"
    uint32_t audioStartGraceMs      = 250;   // a word that never shows up as audio busy within this time is silent
    uint32_t audioMaxWaitMs         = 2500;  // never wait longer than this for a sound to finish
    uint32_t firstChantTouchGraceMs = 1500;  // first chant after start() / continueGame(): early touches do not
                                             // count (the player who pressed may still stand next to the robot)
    uint32_t turnBackMinMs     = 300;
    uint32_t turnBackTimeoutMs = 1500;
    uint32_t settleMs          = 700;    // Watching also waits until the "木头人" melody has finished
    uint32_t watchBaseMs       = 2200;
    uint32_t watchPerRoundMs   = 300;
    uint32_t watchMaxMs        = 4000;
    uint32_t survivedMs        = 1500;
    uint32_t caughtMs          = 4000;
    bool continueAfterCaught   = true;   // party elimination: wait in CaughtWait instead of ending the game
    uint32_t caughtWaitTimeoutMs = 90000;  // nobody continues: back to Idle (0 = wait forever)
    uint32_t winMs             = 4500;
    float motionThreshold      = 0.04f;  // motion score (fraction of changed cells) that counts as moving
    float thresholdDecay       = 0.92f;  // threshold multiplier per survived round
    float thresholdFloor       = 0.015f;
    uint8_t confirmFrames      = 2;      // consecutive new frames above threshold needed to catch
    uint8_t cellThreshold      = 24;     // passed to MotionDetector
};

struct FreezeInput {
    uint32_t nowMs       = 0;
    bool headTouched     = false;    // edge: the head was touched since the previous update()
    bool servoMoving     = false;
    bool audioBusy       = false;    // SoundPlayer::isBusy(): a chant word / melody is still playing
    const uint8_t* luma  = nullptr;  // latest luma frame, may repeat between updates (see lumaSeq)
    int lumaWidth        = 0;
    int lumaHeight       = 0;
    uint32_t lumaSeq     = 0;        // only a frame with a seq different from the last seen one is analysed
};

enum class FreezeEventType : uint8_t {
    PhaseEntered,   // phase
    ChantWord,      // index: 0 = "一", 1 = "二", 2 = "三", 3 = "木头人"
    Caught,         // x, y: normalized image position (-1..1) of the largest moving blob
    RoundSurvived,  // round: number of rounds survived so far
};

struct FreezeEvent {
    FreezeEventType type = FreezeEventType::PhaseEntered;
    FreezePhase phase    = FreezePhase::Idle;
    int index            = 0;
    int round            = 0;
    float x              = 0.0f;
    float y              = 0.0f;
};

class FreezeGame {
public:
    explicit FreezeGame(const FreezeConfig& config = {});

    void start(uint32_t nowMs);  // from any phase -> Ready, resets the round counter
    // CaughtWait -> Chanting: the caught player is out, the others survived that round (round + 1).
    // Returns false (and does nothing) in any other phase.
    bool continueGame(uint32_t nowMs);
    // True once continueGame() was used in the current game (until the next start()): such a game must not
    // count for the "most rounds" record, otherwise one player could farm it with caught -> continue.
    bool hasContinued() const;
    void reset();                // -> Idle without events
    void update(const FreezeInput& input);

    FreezePhase phase() const;
    uint32_t phaseStartMs() const;
    int round() const;               // rounds survived in the current game
    int bestRound() const;           // best result since construction
    float motionLevel() const;       // latest motion score while Watching (0..1), otherwise 0
    float currentThreshold() const;  // threshold for the current round
    uint32_t watchDurationMs() const;

    std::vector<FreezeEvent> takeEvents();

private:
    void enter(FreezePhase phase, uint32_t nowMs);
    void start_chanting(uint32_t nowMs);
    void start_audio_wait(uint32_t nowMs);
    bool audio_finished(const FreezeInput& input);
    uint32_t next_random();
    uint32_t next_chant_gap();

    FreezeConfig _config;
    vision::MotionDetector _detector;
    FreezePhase _phase        = FreezePhase::Idle;
    uint32_t _phase_start_ms  = 0;
    uint32_t _next_word_ms    = 0;
    int _word_index           = 0;
    bool _waiting_audio       = false;
    bool _grace_next_chant    = false;  // the next Chanting phase gets the touch grace
    bool _grace_chant         = false;  // the current Chanting phase has the touch grace
    bool _continued           = false;
    bool _audio_seen_busy     = false;
    uint32_t _audio_wait_ms   = 0;
    uint32_t _final_word_ms   = 0;
    int _round                = 0;
    int _best_round           = 0;
    float _motion_level       = 0.0f;
    int _frames_above         = 0;
    uint32_t _last_luma_seq   = 0;
    bool _has_last_luma_seq   = false;
    uint32_t _rng_state       = 1;
    std::vector<FreezeEvent> _events;
};

}  // namespace stackpet::games
