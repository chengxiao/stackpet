/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "freeze_game.h"
#include <algorithm>
#include <cmath>

namespace stackpet::games {

static constexpr int kChantWordCount = 4;  // "一", "二", "三", "木头人"

FreezeGame::FreezeGame(const FreezeConfig& config) : _config(config)
{
    _rng_state = _config.seed != 0 ? _config.seed : 1;
    _detector.setCellThreshold(_config.cellThreshold);
}

void FreezeGame::start(uint32_t nowMs)
{
    _round             = 0;
    _word_index        = 0;
    _waiting_audio     = false;
    _grace_next_chant  = true;
    _continued         = false;
    _frames_above      = 0;
    _motion_level      = 0.0f;
    _detector.clearReference();
    enter(FreezePhase::Ready, nowMs);
}

bool FreezeGame::continueGame(uint32_t nowMs)
{
    if (_phase != FreezePhase::CaughtWait) {
        return false;
    }
    // Everybody who was not caught survived that round
    _round++;
    _best_round       = std::max(_best_round, _round);
    _frames_above     = 0;
    _motion_level     = 0.0f;
    _continued        = true;
    // Whoever walked up to tap "continue" is right next to the robot: no instant win when it turns away
    _grace_next_chant = true;
    _detector.clearReference();
    start_chanting(nowMs);
    return true;
}

bool FreezeGame::hasContinued() const
{
    return _continued;
}

void FreezeGame::reset()
{
    _phase            = FreezePhase::Idle;
    _round            = 0;
    _word_index       = 0;
    _waiting_audio    = false;
    _grace_next_chant = false;
    _grace_chant      = false;
    _continued        = false;
    _frames_above  = 0;
    _motion_level = 0.0f;
    _detector.clearReference();
    _events.clear();
}

void FreezeGame::update(const FreezeInput& input)
{
    const uint32_t now = input.nowMs;
    const bool new_frame =
        input.luma != nullptr && (!_has_last_luma_seq || input.lumaSeq != _last_luma_seq);

    // Tagging the robot wins at any moment it is not looking yet, or while it stares (if it was not caught first).
    // Right after start() / continueGame() a player may still be next to the robot, so early touches in that
    // first chant do not count.
    const bool touch_grace = _phase == FreezePhase::Chanting && _grace_chant &&
                             now - _phase_start_ms < _config.firstChantTouchGraceMs;
    if (input.headTouched && !touch_grace) {
        switch (_phase) {
            case FreezePhase::Chanting:
            case FreezePhase::TurningBack:
            case FreezePhase::Settling:
            case FreezePhase::Watching:
            case FreezePhase::Survived:
                _best_round   = std::max(_best_round, _round);
                _motion_level = 0.0f;
                enter(FreezePhase::PlayerWin, now);
                break;
            default:
                break;
        }
    }

    // Measured after the touch check, so a phase entered above starts from zero
    const uint32_t elapsed = now - _phase_start_ms;

    switch (_phase) {
        case FreezePhase::Idle:
            break;

        case FreezePhase::Ready:
            if (elapsed >= _config.readyMs) {
                start_chanting(now);
            }
            break;

        case FreezePhase::Chanting:
            if (_word_index >= kChantWordCount) {
                // "木头人" is playing: snap back on its last note
                if (now - _final_word_ms >= _config.finalWordTurnDelayMs) {
                    enter(FreezePhase::TurningBack, now);
                }
                break;
            }
            if (_waiting_audio) {
                // The gap only starts once the previous word has been heard completely
                if (audio_finished(input)) {
                    _waiting_audio = false;
                    _next_word_ms  = now + next_chant_gap();
                }
                break;
            }
            if (static_cast<int32_t>(now - _next_word_ms) >= 0) {
                FreezeEvent event;
                event.type  = FreezeEventType::ChantWord;
                event.phase = _phase;
                event.index = _word_index;
                event.round = _round;
                _events.push_back(event);

                _word_index++;
                if (_word_index >= kChantWordCount) {
                    _final_word_ms = now;
                } else {
                    start_audio_wait(now);
                }
            }
            break;

        case FreezePhase::TurningBack:
            if ((elapsed >= _config.turnBackMinMs && !input.servoMoving) || elapsed >= _config.turnBackTimeoutMs) {
                enter(FreezePhase::Settling, now);
            }
            break;

        case FreezePhase::Settling:
            // Do not start watching while "木头人" is still being sung
            if (elapsed >= _config.settleMs && (!input.audioBusy || elapsed >= _config.settleMs + _config.audioMaxWaitMs)) {
                _detector.clearReference();
                _frames_above = 0;
                _motion_level = 0.0f;
                enter(FreezePhase::Watching, now);
            }
            break;

        case FreezePhase::Watching: {
            if (new_frame) {
                if (!_detector.hasReference()) {
                    // First frame after settling is the "frozen" scene
                    _detector.setReference(input.luma, input.lumaWidth, input.lumaHeight);
                } else {
                    auto result   = _detector.compare(input.luma, input.lumaWidth, input.lumaHeight);
                    _motion_level = result.valid ? result.score : 0.0f;
                    if (result.valid && result.score > currentThreshold()) {
                        _frames_above++;
                    } else {
                        _frames_above = 0;
                    }

                    if (_frames_above >= std::max<int>(1, _config.confirmFrames)) {
                        _best_round = std::max(_best_round, _round);

                        FreezeEvent event;
                        event.type  = FreezeEventType::Caught;
                        event.phase = _phase;
                        event.round = _round;
                        // Look at the biggest mover, not the average of everybody who moved
                        event.x = result.blobCells > 0 ? result.blobCx : result.cx;
                        event.y = result.blobCells > 0 ? result.blobCy : result.cy;
                        _events.push_back(event);

                        enter(FreezePhase::Caught, now);
                        break;
                    }
                }
            }

            if (elapsed >= watchDurationMs()) {
                _round++;
                _best_round   = std::max(_best_round, _round);
                _motion_level = 0.0f;

                FreezeEvent event;
                event.type  = FreezeEventType::RoundSurvived;
                event.phase = _phase;
                event.round = _round;
                _events.push_back(event);

                enter(FreezePhase::Survived, now);
            }
            break;
        }

        case FreezePhase::Survived:
            if (elapsed >= _config.survivedMs) {
                start_chanting(now);
            }
            break;

        case FreezePhase::Caught:
            if (elapsed >= _config.caughtMs) {
                _motion_level = 0.0f;
                enter(_config.continueAfterCaught ? FreezePhase::CaughtWait : FreezePhase::Idle, now);
            }
            break;

        case FreezePhase::CaughtWait:
            if (_config.caughtWaitTimeoutMs > 0 && elapsed >= _config.caughtWaitTimeoutMs) {
                enter(FreezePhase::Idle, now);
            }
            break;

        case FreezePhase::PlayerWin:
            if (elapsed >= _config.winMs) {
                enter(FreezePhase::Idle, now);
            }
            break;
    }

    if (new_frame) {
        _has_last_luma_seq = true;
        _last_luma_seq     = input.lumaSeq;
    }
}

FreezePhase FreezeGame::phase() const
{
    return _phase;
}

uint32_t FreezeGame::phaseStartMs() const
{
    return _phase_start_ms;
}

int FreezeGame::round() const
{
    return _round;
}

int FreezeGame::bestRound() const
{
    return _best_round;
}

float FreezeGame::motionLevel() const
{
    return _phase == FreezePhase::Watching ? _motion_level : 0.0f;
}

float FreezeGame::currentThreshold() const
{
    float threshold = _config.motionThreshold * std::pow(_config.thresholdDecay, static_cast<float>(_round));
    return std::max(threshold, _config.thresholdFloor);
}

uint32_t FreezeGame::watchDurationMs() const
{
    uint32_t duration = _config.watchBaseMs + _config.watchPerRoundMs * static_cast<uint32_t>(_round);
    return std::min(duration, _config.watchMaxMs);
}

std::vector<FreezeEvent> FreezeGame::takeEvents()
{
    std::vector<FreezeEvent> events;
    events.swap(_events);
    return events;
}

void FreezeGame::enter(FreezePhase phase, uint32_t nowMs)
{
    _phase          = phase;
    _phase_start_ms = nowMs;

    FreezeEvent event;
    event.type  = FreezeEventType::PhaseEntered;
    event.phase = phase;
    event.round = _round;
    _events.push_back(event);
}

void FreezeGame::start_chanting(uint32_t nowMs)
{
    _word_index       = 0;
    _grace_chant      = _grace_next_chant;
    _grace_next_chant = false;
    // The first word also waits for whatever is still playing (countdown tick, "next round" jingle)
    start_audio_wait(nowMs);
    enter(FreezePhase::Chanting, nowMs);
}

void FreezeGame::start_audio_wait(uint32_t nowMs)
{
    _waiting_audio   = true;
    _audio_seen_busy = false;
    _audio_wait_ms   = nowMs;
}

bool FreezeGame::audio_finished(const FreezeInput& input)
{
    const uint32_t waited = input.nowMs - _audio_wait_ms;
    if (input.audioBusy) {
        _audio_seen_busy = true;
    }
    if (waited >= _config.audioMaxWaitMs) {
        return true;
    }
    if (_audio_seen_busy) {
        return !input.audioBusy;
    }
    // Nothing started playing (no sound, or the speaker is off): do not wait forever
    return !input.audioBusy && waited >= _config.audioStartGraceMs;
}

uint32_t FreezeGame::next_random()
{
    // xorshift32
    uint32_t x = _rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    _rng_state = x != 0 ? x : 1;
    return _rng_state;
}

uint32_t FreezeGame::next_chant_gap()
{
    const float scale = std::pow(_config.chantSpeedup, static_cast<float>(_round));
    uint32_t low      = std::max(_config.chantGapFloorMs, static_cast<uint32_t>(_config.chantGapMinMs * scale));
    uint32_t high     = std::max(low, static_cast<uint32_t>(_config.chantGapMaxMs * scale));
    return low + next_random() % (high - low + 1);
}

}  // namespace stackpet::games
