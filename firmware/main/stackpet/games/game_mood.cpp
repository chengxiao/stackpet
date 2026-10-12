/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "game_mood.h"

namespace stackpet::games {

void MoodScript::play(std::vector<MoodBeat> beats, uint32_t nowMs)
{
    if (beats.empty()) {
        return;
    }
    _beats    = std::move(beats);
    _start_ms = nowMs;
}

bool MoodScript::update(uint32_t nowMs, Emotion& out)
{
    if (_beats.empty()) {
        return false;
    }

    // Walk the beats; the last one (or any 0-duration beat) holds
    uint32_t elapsed = nowMs - _start_ms;
    Emotion emotion  = _beats.back().emotion;
    for (const auto& beat : _beats) {
        if (beat.durationMs == 0 || elapsed < beat.durationMs) {
            emotion = beat.emotion;
            break;
        }
        elapsed -= beat.durationMs;
    }

    if (_has_reported && emotion == _reported) {
        return false;
    }
    _has_reported = true;
    _reported     = emotion;
    out           = emotion;
    return true;
}

bool MoodScript::active() const
{
    return !_beats.empty();
}

std::vector<MoodBeat> freezeMood(FreezePhase phase)
{
    switch (phase) {
        case FreezePhase::Idle:
            return {{Emotion::Neutral, 0}};
        case FreezePhase::Ready:
            return {{Emotion::Happy, 0}};
        case FreezePhase::Chanting:
            // Cheeky, eyes squeezed shut: "I'm not peeking!"
            return {{Emotion::Laughing, 0}};
        case FreezePhase::TurningBack:
        case FreezePhase::Settling:
        case FreezePhase::Watching:
            // Suspicious stare (WatchMood takes over while watching)
            return {{Emotion::Doubt, 0}};
        case FreezePhase::Survived:
            // Nobody moved: impressed
            return {{Emotion::Love, 0}};
        case FreezePhase::Caught:
            // Startled, then angry, then a little taunt for the one who is out
            return {{Emotion::Surprised, 600}, {Emotion::Angry, 1400}, {Emotion::Laughing, 0}};
        case FreezePhase::CaughtWait:
            // Keeping an eye on the others: they must not move
            return {{Emotion::Doubt, 0}};
        case FreezePhase::PlayerWin:
            // "You got me!" then adoring the winner
            return {{Emotion::Surprised, 500}, {Emotion::Love, 0}};
    }
    return {{Emotion::Neutral, 0}};
}

void WatchMood::reset(uint32_t nowMs)
{
    _current    = Emotion::Doubt;
    _changed_ms = nowMs;
}

Emotion WatchMood::update(float danger, uint32_t nowMs)
{
    if (nowMs - _changed_ms < kDwellMs) {
        return _current;
    }
    if (_current != Emotion::Surprised && danger >= kAlertOn) {
        _current    = Emotion::Surprised;
        _changed_ms = nowMs;
    } else if (_current == Emotion::Surprised && danger < kAlertOff) {
        _current    = Emotion::Doubt;
        _changed_ms = nowMs;
    }
    return _current;
}

Emotion WatchMood::current() const
{
    return _current;
}

std::vector<MoodBeat> spinMood(SpinPhase phase)
{
    switch (phase) {
        case SpinPhase::Idle:
            return {{Emotion::Neutral, 0}};
        case SpinPhase::SpinUp:
            // "Whoa!" then the thrill of the ride
            return {{Emotion::Surprised, 700}, {Emotion::Laughing, 0}};
        case SpinPhase::SlowDown:
            // Who will it be...
            return {{Emotion::Doubt, 0}};
        case SpinPhase::Cruise:
        case SpinPhase::Settle:
        case SpinPhase::Reveal:
            return {};
    }
    return {};
}

std::vector<MoodBeat> spinRevealMood(SpinTone tone)
{
    // Every reveal starts with a "ta-da" (wide eyes), then reacts to what came out
    constexpr uint32_t kTaDaMs = 450;
    switch (tone) {
        case SpinTone::Pick:
            return {{Emotion::Surprised, kTaDaMs}, {Emotion::Laughing, 0}};
        case SpinTone::Food:
            return {{Emotion::Surprised, kTaDaMs}, {Emotion::Love, 0}};
        case SpinTone::Truth:
            // An embarrassing question: blushing for the victim
            return {{Emotion::Surprised, kTaDaMs}, {Emotion::Shy, 0}};
        case SpinTone::Dare:
            // A punishment: cheeky
            return {{Emotion::Surprised, kTaDaMs}, {Emotion::Laughing, 0}};
        case SpinTone::Yes:
            return {{Emotion::Surprised, kTaDaMs}, {Emotion::Love, 0}};
        case SpinTone::No:
            return {{Emotion::Surprised, kTaDaMs}, {Emotion::Doubt, 0}};
        case SpinTone::Joke:
            return {{Emotion::Surprised, kTaDaMs}, {Emotion::Laughing, 0}};
    }
    return {{Emotion::Happy, 0}};
}

}  // namespace stackpet::games
