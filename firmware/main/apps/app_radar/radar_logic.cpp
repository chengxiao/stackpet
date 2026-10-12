/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "radar_logic.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace stackpet::radar {

namespace {

constexpr float kDegToRad        = 3.14159265f / 180.0f;
constexpr size_t kHistorySize    = 96;  // >= 1.9 s of head positions at the 20 ms recording period
constexpr uint32_t kRecordPeriod = 20;
constexpr uint32_t kMaxStepMs    = 100;  // a stalled main loop must not teleport the sweep
constexpr uint32_t kFirstPingMs  = 400;
constexpr float kFaceWidthM      = 0.16f;
constexpr uint32_t kCheckTurnMs  = 700;  // spin scan: braking and turning back to a face before the settle time
constexpr uint32_t kStepTimeoutMs = 2000;  // spin scan: a look-around stop not reached by then (stalled head)
constexpr uint32_t kMaxTrackStepMs = 500;  // SpinTracker: longer gaps than this are not modelled
constexpr uint32_t kBrakeTickMs    = 40;   // spin scan: the next tick may come this late at least ...
constexpr uint32_t kBrakeMarginMs  = 30;   // ... and the estimate lag behind (leaving the dead band)
constexpr uint32_t kBrakeStartMs   = 60;   // a look-around step from rest: starting up before it gets going

// Wrap-safe "now is at or after `at`"
bool reached(uint32_t now, uint32_t at)
{
    return static_cast<int32_t>(now - at) >= 0;
}

uint32_t elapsed(uint32_t now, uint32_t since)
{
    int32_t d = static_cast<int32_t>(now - since);
    return d > 0 ? static_cast<uint32_t>(d) : 0;
}

int clamp_int(int v, int lo, int hi)
{
    return std::min(std::max(v, lo), hi);
}

}  // namespace

/* -------------------------------------------------------------------------- */
/*                                SweepPlanner                                */
/* -------------------------------------------------------------------------- */
SweepPlanner::SweepPlanner(const Config& config) : _config(config)
{
}

void SweepPlanner::setConfig(const Config& config)
{
    _config   = config;
    _position = std::clamp(_position, -static_cast<float>(std::max(config.limit, 0)),
                           static_cast<float>(std::max(config.limit, 0)));
    if (_config.dwellEvery <= 0 && _state != State::Moving) {
        _state    = State::Moving;
        _traveled = 0.0f;
    }
}

const SweepPlanner::Config& SweepPlanner::config() const
{
    return _config;
}

void SweepPlanner::setSpeed(int speed)
{
    _config.speed = speed;
}

void SweepPlanner::reset(float position, int direction)
{
    const float limit = static_cast<float>(std::max(_config.limit, 0));
    _position         = std::clamp(position, -limit, limit);
    _velocity         = 0.0f;
    _direction        = direction >= 0 ? 1 : -1;
    _state            = State::Moving;
    _traveled         = 0.0f;
    _has_time         = false;
}

void SweepPlanner::update(uint32_t nowMs)
{
    if (!_has_time) {
        _has_time = true;
        _last_ms  = nowMs;
        return;
    }
    uint32_t dt_ms = elapsed(nowMs, _last_ms);
    _last_ms       = nowMs;
    if (dt_ms == 0) {
        return;
    }
    dt_ms          = std::min(dt_ms, kMaxStepMs);
    const float dt = static_cast<float>(dt_ms) / 1000.0f;

    const float limit = static_cast<float>(std::max(_config.limit, 0));
    const float speed = static_cast<float>(std::max(_config.speed, 1));
    const float accel = static_cast<float>(std::max(_config.accel, 1));

    if (_state == State::Dwelling) {
        if (elapsed(nowMs, _dwell_from) < _config.dwellMs) {
            return;
        }
        _state    = State::Moving;
        _traveled = 0.0f;
    }

    float desired = 0.0f;
    if (_state == State::Moving) {
        // Turn around when the distance left is just what it takes to brake to zero at the limit. A head at
        // rest on the limit (v_towards == 0) turns around too.
        const float remaining = limit - _position * static_cast<float>(_direction);
        const float v_towards = _velocity * static_cast<float>(_direction);
        if (v_towards >= 0.0f && remaining <= v_towards * v_towards / (2.0f * accel) + 0.5f) {
            _direction = -_direction;
        }
        desired = static_cast<float>(_direction) * speed;
    }

    const float max_dv = accel * dt;
    _velocity += std::clamp(desired - _velocity, -max_dv, max_dv);
    const float step = _velocity * dt;
    _position += step;
    if (_position > limit) {
        _position = limit;
        _velocity = std::min(_velocity, 0.0f);
    } else if (_position < -limit) {
        _position = -limit;
        _velocity = std::max(_velocity, 0.0f);
    }
    _traveled += std::fabs(step);

    if (_state == State::Moving && _config.dwellEvery > 0 && _traveled >= static_cast<float>(_config.dwellEvery)) {
        _state = State::Braking;
    }
    if (_state == State::Braking && std::fabs(_velocity) < 1e-3f) {
        _velocity   = 0.0f;
        _state      = State::Dwelling;
        _dwell_from = nowMs;
    }
}

float SweepPlanner::position() const
{
    return _position;
}

float SweepPlanner::velocity() const
{
    return _velocity;
}

int SweepPlanner::direction() const
{
    return _direction;
}

bool SweepPlanner::dwelling() const
{
    return _state == State::Dwelling;
}

uint32_t SweepPlanner::dwellStartMs() const
{
    return _dwell_from;
}

/* -------------------------------------------------------------------------- */
/*                                RadarMachine                                */
/* -------------------------------------------------------------------------- */
const char* phaseName(Phase phase)
{
    switch (phase) {
        case Phase::Scanning:
            return "scanning";
        case Phase::Locking:
            return "locking";
        case Phase::Angry:
            return "angry";
        case Phase::Cooldown:
            return "cooldown";
        case Phase::Forgiven:
            return "forgiven";
        case Phase::Checking:
            return "checking";
    }
    return "?";
}

RadarMachine::RadarMachine(const RadarConfig& config) : _config(config), _planner(config.sweep)
{
    _history.resize(kHistorySize);
}

void RadarMachine::start(uint32_t nowMs, int yaw, int pitch)
{
    _events.clear();
    _history_count   = 0;
    _history_head    = 0;
    _last_record_ms  = 0;
    _locked_once     = false;
    _alarm_cancelled = false;
    _friend_active   = false;
    _target_yaw      = clamp_int(yaw, -_config.aimYawLimit, _config.aimYawLimit);
    _target_pitch    = clamp_int(pitch, _config.pitchMin, _config.pitchMax);
    _last_move_ms    = nowMs;
    _last_seen_ms    = nowMs;
    enter(Phase::Scanning, nowMs);
}

void RadarMachine::update(const RadarInput& input)
{
    const uint32_t now = input.nowMs;
    const Contact& c   = input.contact;
    const bool sighted = c.fresh && c.present;
    if (sighted) {
        _last_seen_ms = now;
    }
    if (_friend_active && sighted) {
        _friend_until = now + _config.friendGraceMs;  // still around: the grace lasts until nobody is seen
    }
    if (_friend_active && reached(now, _friend_until)) {
        _friend_active = false;
    }
    if (input.headValid) {
        _head_valid    = true;
        _head_yaw      = input.headYaw;
        _head_velocity = input.headVelocity;
    }
    _tick_ms        = _updated ? std::min(elapsed(now, _last_update_ms), kMaxStepMs) : 0;
    _last_update_ms = now;
    _updated        = true;

    switch (_phase) {
        case Phase::Scanning: {
            if (_config.spinScan) {
                update_spin_scan(input);
                break;
            }
            _planner.update(now);
            _head.stream  = true;
            _head.yaw     = static_cast<int>(std::lround(_planner.position()));
            _head.pitch   = _config.scanPitch;
            _target_yaw   = _head.yaw;
            _target_pitch = _head.pitch;
            record(now, _head.yaw, _head.pitch, false);

            if (reached(now, _next_ping_ms)) {
                _next_ping_ms = now + _config.pingPeriodMs;
                RadarEvent ping;
                ping.type = RadarEventType::Ping;
                _events.push_back(ping);
            }

            if (sighted) {
                emit_blip(c, now);
                if (confirmed(c) && !inFriendGrace(now)) {
                    aim_at(c, now, _config.aimSpeed);
                    enter(Phase::Locking, now);
                }
            }
            break;
        }

        case Phase::Locking:
            if (input.petted) {
                enter(Phase::Forgiven, now);
                break;
            }
            track(c, now);
            if (elapsed(now, _phase_start) >= _config.lockViewMs) {
                enter(Phase::Angry, now);
            }
            break;

        case Phase::Angry:
            if (input.petted) {
                enter(Phase::Forgiven, now);
                break;
            }
            track(c, now);
            // Keep glaring while the target stays, calm down once it has been gone for a while
            if (elapsed(now, _phase_start) >= _config.glareMs && elapsed(now, _last_seen_ms) >= _config.lostMs) {
                enter(Phase::Cooldown, now);
            }
            break;

        case Phase::Cooldown:
            if (input.petted) {
                enter(Phase::Forgiven, now);
                break;
            }
            if (sighted && confirmed(c) && !inFriendGrace(now)) {
                // It came back: angry again (no second alarm)
                aim_at(c, now, _config.aimSpeed);
                enter(Phase::Angry, now);
                break;
            }
            if (elapsed(now, _phase_start) >= _config.cooldownMs) {
                enter(Phase::Scanning, now);
            }
            break;

        case Phase::Forgiven:
            if (elapsed(now, _phase_start) >= _config.forgivenMs) {
                enter(Phase::Cooldown, now);
            }
            break;

        case Phase::Checking:
            if (input.headValid) {
                // Still in wheel mode (leaving the sensor's dead band first): the turn to the face has not started
                _last_move_ms   = now;
                _check_deadline = now + kCheckTurnMs + _config.settleMs + _config.checkMs;
            }
            // Only frames taken after the head came to rest count: two in a row with the face lock on, a still frame
            // without it starts over. Nothing confirmed in time: spin on.
            if (c.fresh && static_cast<int32_t>(c.captureMs - _last_move_ms) >= static_cast<int32_t>(_config.settleMs)) {
                _check_hits = c.present ? _check_hits + 1 : 0;
                if (c.present && _check_hits >= _config.confirmHits && !inFriendGrace(now)) {
                    aim_at(c, now, _config.aimSpeed);
                    enter(Phase::Locking, now);
                    break;
                }
            }
            if (reached(now, _check_deadline)) {
                mute(_check_bearing, now);  // not confirmed: do not brake for that bearing again for a while
                enter(Phase::Scanning, now);
            }
            break;
    }
}

void RadarMachine::update_spin_scan(const RadarInput& input)
{
    const uint32_t now = input.nowMs;
    const Contact& c   = input.contact;
    if (input.headValid) {
        record(now, _head_yaw, _config.scanPitch, false, true);
        if (_last_head_set) {
            // Signed, the spin's way round: an estimate that jumps ahead and back (glitches, a stuck head) adds nothing
            _fast_travel += static_cast<float>((_head_yaw - _last_head_yaw) * _spin_direction);
        }
        _last_head_yaw = _head_yaw;
        _last_head_set = true;
    }

    if (reached(now, _next_ping_ms)) {
        _next_ping_ms = now + _config.pingPeriodMs;
        RadarEvent ping;
        ping.type = RadarEventType::Ping;
        _events.push_back(ping);
    }

    switch (_spin_stage) {
        case SpinStage::Fast:
            if (_fast_travel >= _config.spinTurns * 3600.0f) {
                start_look_around(now);
            }
            break;
        case SpinStage::Step:
            arm_brake(now, input.headValid);
            break;
        case SpinStage::Stop:
            if (c.fresh && static_cast<int32_t>(c.captureMs - _stop_ms) >= static_cast<int32_t>(_config.lookSettleMs)) {
                _still_frames++;
            }
            if (_still_frames >= _config.lookFrames || elapsed(now, _stop_ms) >= _config.lookMaxMs) {
                next_stop(now);
            }
            break;
    }

    if (c.fresh && c.present) {
        const int bearing = bearingOf(c);
        emit_blip(c, now);
        if (!inFriendGrace(now) && reachable(bearing) && !muted(bearing, now)) {
            // Where the face was when the frame was taken; Checking turns there and confirms it on still frames
            _check_bearing = bearing;
            _check_yaw     = bearing;
            _check_pitch   = pitchAt(c.captureMs) + pitchOffsetFor(c.y);
            enter(Phase::Checking, now);
        }
    }
}

bool RadarMachine::reachable(int bearing) const
{
    const float reach = static_cast<float>(_config.aimYawLimit) + _config.reachFov * static_cast<float>(_config.halfFovYaw);
    return std::abs(static_cast<float>(wrapYaw(bearing))) <= reach;
}

bool RadarMachine::muted(int bearing, uint32_t nowMs) const
{
    for (const auto& m : _muted) {
        if (m.active && !reached(nowMs, m.until) && std::abs(wrapYaw(bearing - m.bearing)) <= _config.checkMuteYaw) {
            return true;
        }
    }
    return false;
}

void RadarMachine::mute(int bearing, uint32_t nowMs)
{
    // Replace an expired entry, else the one that expires first
    MutedBearing* slot = &_muted[0];
    for (auto& m : _muted) {
        if (!m.active || reached(nowMs, m.until)) {
            slot = &m;
            break;
        }
        if (static_cast<int32_t>(m.until - slot->until) < 0) {
            slot = &m;
        }
    }
    slot->bearing = bearing;
    slot->until   = nowMs + _config.checkMuteMs;
    slot->active  = true;
}

void RadarMachine::start_fast_turns()
{
    _spin_stage  = SpinStage::Fast;
    _fast_travel = 0.0f;
    set_spin_command(_config.spinVelocity);
}

void RadarMachine::start_look_around(uint32_t nowMs)
{
    _stop_index = 0;
    _stop_yaw   = _head_yaw + _spin_direction * (std::max(_config.lookLeadYaw, 1) - 1);
    next_stop(nowMs);
}

void RadarMachine::next_stop(uint32_t nowMs)
{
    if (_stop_index >= _config.lookStops) {
        start_fast_turns();
        return;
    }
    _stop_index++;
    _stop_yaw   = look_stop_after(_stop_yaw);
    _spin_stage = SpinStage::Step;
    _stop_ms    = nowMs;
    set_spin_command(std::min(_config.lookVelocity, _config.spinVelocity));
    arm_brake(nowMs, _head_valid);  // the next tick may well come after the brake point already
}

bool RadarMachine::arm_brake(uint32_t nowMs, bool headValid)
{
    // Stop rotating before the stop: the head freewheels on (lookCoastMs worth at its speed). Ticks are long and
    // uneven (the CoreS3's RADAR loop shares its core with the face detector: 55 ms, now and then 150, 18 degree and
    // more at this speed): once the brake point comes before the next tick could (taken as at least kBrakeTickMs away,
    // at the speed the head is getting to), the app turns the head on to it watching the sensor, then stops it. A
    // stalled head stops anyway, after a while.
    const int velocity   = _head.velocity;
    const float speed    = std::max(_head_velocity * static_cast<float>(_spin_direction), 0.0f);
    const float expected = std::max(speed, static_cast<float>(std::abs(velocity)) * _config.spinRatePerUnit);
    const int coast      = static_cast<int>(std::lround(expected * static_cast<float>(_config.lookCoastMs) / 1000.0f));
    const int brake_at   = _stop_yaw - _spin_direction * coast;
    const float to_go    = static_cast<float>((brake_at - _head_yaw) * _spin_direction);
    const bool soon      = headValid && to_go <= expected *
                                                 static_cast<float>(std::max(_tick_ms, kBrakeTickMs) + kBrakeMarginMs) /
                                                 1000.0f;
    if (!soon && elapsed(nowMs, _stop_ms) < kStepTimeoutMs) {
        return false;
    }
    _spin_stage   = SpinStage::Stop;
    _still_frames = 0;
    set_spin_command(0);
    _stop_ms = nowMs;
    if (soon && to_go > 0.0f && expected > 0.0f) {
        _head.brakeArmed    = true;
        _head.brakeAtYaw    = brake_at;
        _head.brakeVelocity = velocity;
        _head.brakeSerial++;
        // When it will brake (from rest it takes a while to get going)
        _stop_ms += static_cast<uint32_t>(to_go / expected * 1000.0f) + (speed < expected / 2.0f ? kBrakeStartMs : 0);
    }
    return true;
}

int RadarMachine::lookStopYaw(int index) const
{
    const int n = std::max(_config.lookStops, 1);
    int span    = _config.lookToYaw - _config.lookFromYaw;
    span        = ((span % 3600) + 3600) % 3600;
    if (span == 0) {
        span = 3600;
    }
    // Both ends are stops, unless the range is the whole turn
    const int gaps = span >= 3600 || n == 1 ? n : n - 1;
    return wrapYaw(_config.lookFromYaw + span * index / gaps);
}

int RadarMachine::look_stop_after(int yaw) const
{
    int best = 3600;
    for (int i = 0; i < std::max(_config.lookStops, 1); i++) {
        // How far on the spin's way round this stop is (a whole turn when the head is right at it)
        int ahead = (wrapYaw(lookStopYaw(i) - yaw) * _spin_direction + 3600) % 3600;
        if (ahead == 0) {
            ahead = 3600;
        }
        best = std::min(best, ahead);
    }
    return yaw + _spin_direction * best;
}

void RadarMachine::setLookRange(int fromYaw, int toYaw)
{
    _config.lookFromYaw = wrapYaw(fromYaw);
    _config.lookToYaw   = wrapYaw(toYaw);
}

void RadarMachine::cancelCheck(uint32_t nowMs)
{
    if (_phase != Phase::Checking) {
        return;
    }
    mute(_check_bearing, nowMs);
    enter(Phase::Scanning, nowMs);
}

std::vector<RadarEvent> RadarMachine::takeEvents()
{
    std::vector<RadarEvent> out;
    out.swap(_events);
    return out;
}

Phase RadarMachine::phase() const
{
    return _phase;
}

uint32_t RadarMachine::phaseStartMs() const
{
    return _phase_start;
}

const HeadCommand& RadarMachine::head() const
{
    return _head;
}

const RadarConfig& RadarMachine::config() const
{
    return _config;
}

float RadarMachine::sweepYaw() const
{
    if (_config.spinScan) {
        return static_cast<float>(_head_valid ? wrapYaw(_head_yaw) : _target_yaw);
    }
    return _planner.position();
}

float RadarMachine::sweepVelocity() const
{
    if (_config.spinScan) {
        return _phase == Phase::Scanning ? _head_velocity : 0.0f;
    }
    return _planner.velocity();
}

int RadarMachine::sweepDirection() const
{
    if (_config.spinScan) {
        return _spin_direction;
    }
    float v = _planner.velocity();
    if (v > 1.0f) {
        return 1;
    }
    if (v < -1.0f) {
        return -1;
    }
    return _planner.direction();
}

bool RadarMachine::dwellSettled(uint32_t nowMs) const
{
    return _phase == Phase::Scanning && _planner.dwelling() &&
           elapsed(nowMs, _planner.dwellStartMs()) >= _config.settleMs;
}

bool RadarMachine::alarmActive(uint32_t nowMs) const
{
    return _locked_once && !_alarm_cancelled && elapsed(nowMs, _lock_start_ms) < _config.alarmMs;
}

bool RadarMachine::inFriendGrace(uint32_t nowMs) const
{
    return _friend_active && !reached(nowMs, _friend_until);
}

uint32_t RadarMachine::lockStartMs() const
{
    return _lock_start_ms;
}

void RadarMachine::setSweepSpeed(int speed)
{
    _config.sweep.speed = speed;
    _planner.setSpeed(speed);
}

void RadarMachine::setDwellScan(int dwellEvery, uint32_t dwellMs)
{
    _config.sweep.dwellEvery = dwellEvery;
    _config.sweep.dwellMs    = dwellMs;
    _planner.setConfig(_config.sweep);
}

void RadarMachine::setSpinScan(bool enabled, uint32_t nowMs)
{
    if (_config.spinScan == enabled) {
        return;
    }
    _config.spinScan = enabled;
    if (_phase == Phase::Scanning || _phase == Phase::Checking) {
        if (!enabled && _head_valid) {
            // The sweep starts where the spin left the head
            _target_yaw = clamp_int(wrapYaw(_head_yaw), -_config.sweep.limit, _config.sweep.limit);
        }
        _head.rotate = false;
        enter(Phase::Scanning, nowMs);
    }
}

void RadarMachine::setSpinVelocity(int velocity)
{
    _config.spinVelocity = clamp_int(velocity, 0, 1000);
    if (!_head.rotate) {
        return;
    }
    switch (_spin_stage) {
        case SpinStage::Fast:
            set_spin_command(_config.spinVelocity);
            break;
        case SpinStage::Step:
            set_spin_command(std::min(_config.lookVelocity, _config.spinVelocity));
            break;
        case SpinStage::Stop:
            break;
    }
}

int RadarMachine::spinDirection() const
{
    return _spin_direction;
}

bool RadarMachine::lookingAround() const
{
    return _config.spinScan && _phase == Phase::Scanning && _spin_stage != SpinStage::Fast;
}

void RadarMachine::set_spin_command(int speed)
{
    _head.rotate     = true;
    _head.velocity   = _spin_direction * speed * (_config.spinVelocityPerYaw >= 0 ? 1 : -1);
    _head.brakeArmed = false;
}

int RadarMachine::yawOffsetFor(float imageX) const
{
    // Image x is proportional to tan(angle), not to the angle
    const float x     = std::clamp(imageX, -1.5f, 1.5f);
    const float half  = static_cast<float>(_config.halfFovYaw) / 10.0f * kDegToRad;
    const float angle = std::atan(x * std::tan(half)) / kDegToRad * 10.0f;
    return static_cast<int>(std::lround(angle)) * (_config.imageXToYaw >= 0 ? 1 : -1);
}

int RadarMachine::pitchOffsetFor(float imageY) const
{
    const float y     = std::clamp(imageY, -1.5f, 1.5f);
    const float half  = static_cast<float>(_config.halfFovPitch) / 10.0f * kDegToRad;
    const float angle = std::atan(y * std::tan(half)) / kDegToRad * 10.0f;
    return static_cast<int>(std::lround(angle)) * (_config.imageYToPitch >= 0 ? 1 : -1);
}

int RadarMachine::yawAt(uint32_t ms) const
{
    if (_history_count == 0) {
        return _target_yaw;
    }
    // Commanded sweep positions: the head trails them; measured spin readings: the frame was exposed before its stamp
    const HistoryEntry& newest = _history[(_history_head + kHistorySize - 1) % kHistorySize];
    const int lag      = newest.measured ? _config.frameLatencyMs : _config.servoLagMs;
    const uint32_t t   = ms - static_cast<uint32_t>(std::max(lag, 0));
    // Walk from the newest entry back to the first one at or before t
    const HistoryEntry* newer = nullptr;
    for (size_t i = 0; i < _history_count; i++) {
        const HistoryEntry& e = _history[(_history_head + kHistorySize - 1 - i) % kHistorySize];
        if (reached(t, e.ms)) {
            if (newer == nullptr || newer->step) {
                return e.yaw;  // newer than the whole history, or the head only jumped later
            }
            const float span = static_cast<float>(elapsed(newer->ms, e.ms));
            const float f    = span > 0.0f ? static_cast<float>(elapsed(t, e.ms)) / span : 0.0f;
            return static_cast<int>(std::lround(e.yaw + (newer->yaw - e.yaw) * f));
        }
        newer = &e;
    }
    return newer->yaw;  // older than the history: best guess is the oldest entry
}

int RadarMachine::pitchAt(uint32_t ms) const
{
    if (_history_count == 0) {
        return _target_pitch;
    }
    const uint32_t t = ms - static_cast<uint32_t>(std::max(_config.servoLagMs, 0));
    const HistoryEntry* newer = nullptr;
    for (size_t i = 0; i < _history_count; i++) {
        const HistoryEntry& e = _history[(_history_head + kHistorySize - 1 - i) % kHistorySize];
        if (reached(t, e.ms)) {
            return e.pitch;  // the pitch only changes in steps (moves), no interpolation needed
        }
        newer = &e;
    }
    return newer->pitch;
}

int RadarMachine::bearingOf(const Contact& contact) const
{
    return wrapYaw(yawAt(contact.captureMs) + yawOffsetFor(contact.x));
}

int RadarMachine::targetYaw() const
{
    return _target_yaw;
}

int RadarMachine::targetPitch() const
{
    return _target_pitch;
}

void RadarMachine::enter(Phase phase, uint32_t nowMs)
{
    _phase       = phase;
    _phase_start = nowMs;

    RadarEvent event;
    event.type  = RadarEventType::PhaseEntered;
    event.phase = phase;
    _events.push_back(event);

    switch (phase) {
        case Phase::Scanning: {
            if (_config.spinScan) {
                // Fast turns first. The history is measured (unwrapped) yaw from here on.
                _history_count = 0;
                _last_head_set = false;
                _head.stream   = false;
                _head.pitch    = _config.scanPitch;
                _head.speed    = _config.returnSpeed;
                _head.serial++;
                _target_pitch = _config.scanPitch;
                _next_ping_ms = nowMs + kFirstPingMs;
                start_fast_turns();
                break;
            }
            // Keep sweeping the way we went, unless that runs straight into a limit
            const int limit = _config.sweep.limit;
            int direction   = _planner.direction();
            if (_target_yaw >= limit - 50) {
                direction = -1;
            } else if (_target_yaw <= -limit + 50) {
                direction = 1;
            }
            _planner.reset(static_cast<float>(_target_yaw), direction);
            _planner.update(nowMs);
            _head.stream  = true;
            _head.yaw     = static_cast<int>(std::lround(_planner.position()));
            _head.pitch   = _config.scanPitch;
            _target_yaw   = _head.yaw;
            _target_pitch = _head.pitch;
            _next_ping_ms = nowMs + kFirstPingMs;
            record(nowMs, _head.yaw, _head.pitch, false);
            break;
        }
        case Phase::Locking:
            _lock_start_ms   = nowMs;
            _locked_once     = true;
            _alarm_cancelled = false;
            _last_seen_ms    = nowMs;
            break;
        case Phase::Angry:
            break;
        case Phase::Cooldown:
            move_to(_target_yaw, _config.scanPitch, _config.returnSpeed, nowMs);
            break;
        case Phase::Forgiven:
            _alarm_cancelled = true;
            _friend_active   = true;
            _friend_until    = nowMs + _config.friendGraceMs;
            break;
        case Phase::Checking:
            // Brake and turn back (the spinning history means nothing once the head moves on its own targets)
            _head.rotate   = false;
            _history_count = 0;
            _check_hits    = 0;
            move_to(_check_yaw, _check_pitch, _config.aimSpeed, nowMs);
            _check_deadline = nowMs + kCheckTurnMs + _config.settleMs + _config.checkMs;
            break;
    }
}

void RadarMachine::aim_at(const Contact& contact, uint32_t nowMs, int speed)
{
    const int yaw   = yawAt(contact.captureMs) + yawOffsetFor(contact.x);
    const int pitch = pitchAt(contact.captureMs) + pitchOffsetFor(contact.y);
    move_to(yaw, pitch, speed, nowMs);
}

void RadarMachine::move_to(int yaw, int pitch, int speed, uint32_t nowMs)
{
    _head.rotate  = false;
    _target_yaw   = clamp_int(yaw, -_config.aimYawLimit, _config.aimYawLimit);
    _target_pitch = clamp_int(pitch, _config.pitchMin, _config.pitchMax);
    _head.stream  = false;
    _head.yaw     = _target_yaw;
    _head.pitch   = _target_pitch;
    _head.speed   = speed;
    _head.serial++;
    _last_move_ms = nowMs;
    // Frames captured from now on show the head somewhere on its way: recorded as the target, which is right
    // once the move has settled (only settled frames are used to correct the aim)
    record(nowMs, _target_yaw, _target_pitch, true);
}

void RadarMachine::track(const Contact& contact, uint32_t nowMs)
{
    if (!contact.fresh || !contact.present) {
        return;
    }
    // Only a frame taken after the head came to rest tells where the target is relative to the aim
    if (static_cast<int32_t>(contact.captureMs - _last_move_ms) < static_cast<int32_t>(_config.settleMs)) {
        return;
    }
    const int err_yaw   = yawOffsetFor(contact.x);
    const int err_pitch = pitchOffsetFor(contact.y);
    if (std::abs(err_yaw) <= _config.trackDeadband && std::abs(err_pitch) <= _config.trackDeadband) {
        return;
    }
    const int yaw   = _target_yaw + static_cast<int>(std::lround(err_yaw * _config.trackGain));
    const int pitch = _target_pitch + static_cast<int>(std::lround(err_pitch * _config.trackGain));
    move_to(yaw, pitch, _config.trackSpeed, nowMs);
}

void RadarMachine::record(uint32_t nowMs, int yaw, int pitch, bool step, bool measured)
{
    // Sweep positions are sampled every kRecordPeriod (interpolated in between), moves are always recorded
    if (!step && _history_count > 0 && elapsed(nowMs, _last_record_ms) < kRecordPeriod) {
        return;
    }
    _history[_history_head] = HistoryEntry{nowMs, yaw, pitch, step, measured};
    _history_head           = (_history_head + 1) % kHistorySize;
    _history_count          = std::min(_history_count + 1, kHistorySize);
    _last_record_ms         = nowMs;
}

bool RadarMachine::confirmed(const Contact& contact) const
{
    return contact.hits >= _config.confirmHits;
}

void RadarMachine::emit_blip(const Contact& contact, uint32_t nowMs)
{
    RadarEvent blip;
    blip.type      = RadarEventType::Blip;
    blip.bearing   = bearingOf(contact);
    blip.size      = contact.w;
    blip.confirmed = confirmed(contact);
    blip.friendly  = inFriendGrace(nowMs);
    blip.unreachable = _config.spinScan && !reachable(blip.bearing);
    _events.push_back(blip);
}

/* -------------------------------------------------------------------------- */
/*                                   Helpers                                  */
/* -------------------------------------------------------------------------- */
int sweepSpeedForRate(float resultsPerSecond, int fullFovYaw, float framesPerView, int minSpeed, int maxSpeed)
{
    if (resultsPerSecond <= 0.0f || framesPerView <= 0.0f) {
        return minSpeed;
    }
    const float speed = static_cast<float>(fullFovYaw) * resultsPerSecond / framesPerView;
    return clamp_int(static_cast<int>(std::lround(speed)), minSpeed, maxSpeed);
}

float faceDistanceMeters(float widthFraction, int halfFovYaw)
{
    const float w    = std::max(widthFraction, 0.01f);
    const float half = static_cast<float>(halfFovYaw) / 10.0f * kDegToRad;
    return kFaceWidthM / (w * 2.0f * std::tan(half));
}

int wrapYaw(int yaw)
{
    int wrapped = (yaw + 1800) % 3600;
    if (wrapped < 0) {
        wrapped += 3600;
    }
    return wrapped - 1800;
}

/* -------------------------------------------------------------------------- */
/*                                 SpinTracker                                */
/* -------------------------------------------------------------------------- */
SpinTracker::SpinTracker(const Config& config) : _config(config)
{
}

void SpinTracker::setConfig(const Config& config)
{
    _config = config;
}

const SpinTracker::Config& SpinTracker::config() const
{
    return _config;
}

void SpinTracker::reset(uint32_t nowMs, int yaw)
{
    _yaw        = static_cast<float>(yaw);
    _taken_yaw  = _yaw;
    _taken_ms   = nowMs;
    _velocity   = 0.0f;
    _last_ms    = nowMs;
    _dead_band  = false;
    _in_band    = false;
    _last_raw   = -1;
    _has_anchor = false;
    _suspects   = 0;
}

void SpinTracker::setCommand(int velocity)
{
    _command = velocity;
}

void SpinTracker::predict(float dt, float& yaw, float& velocity) const
{
    const float target = static_cast<float>(_command) * _config.yawRatePerUnit * (_in_band ? _config.bandRateScale : 1.0f);
    const bool slowing = std::fabs(target) < std::fabs(_velocity) || target * _velocity < 0.0f;
    const float tau    = (slowing ? _config.slowTauMs : _config.commandTauMs) / 1000.0f;
    const float decay  = tau > 0.0f ? std::exp(-dt / tau) : 0.0f;
    yaw                = _yaw + target * dt + (_velocity - target) * tau * (1.0f - decay);
    velocity           = target + (_velocity - target) * decay;
}

void SpinTracker::coast(float dt)
{
    // Nothing to go by: the head goes where the command takes it
    predict(dt, _yaw, _velocity);
    _has_anchor = false;  // the next velocity sample starts at a reading
}

void SpinTracker::take(uint32_t nowMs, float yaw, int raw)
{
    // Velocity from readings at least velocitySpanMs apart: one sensor step is 3 degree/s over a second, 300 over
    // 10 ms
    if (_has_anchor) {
        const uint32_t span = elapsed(nowMs, _anchor_ms);
        if (span >= _config.velocitySpanMs) {
            const float sample = (yaw - _anchor_yaw) * 1000.0f / static_cast<float>(span);
            _velocity += _config.velocityAlpha * (sample - _velocity);
            _anchor_yaw = yaw;
            _anchor_ms  = nowMs;
        }
    } else {
        _has_anchor = true;
        _anchor_yaw = yaw;
        _anchor_ms  = nowMs;
    }
    _velocity  = std::clamp(_velocity, -_config.maxRate, _config.maxRate);
    _yaw       = yaw;
    _taken_yaw = yaw;
    _taken_ms  = nowMs;
    _dead_band = false;
    _last_raw  = raw;
    _suspects  = 0;
}

void SpinTracker::update(uint32_t nowMs, int raw)
{
    // The head turned for as long as it took (a stalled loop too), within reason
    const float dt = static_cast<float>(std::min(elapsed(nowMs, _last_ms), kMaxTrackStepMs)) / 1000.0f;
    _last_ms       = nowMs;

    if (raw < 0) {
        _dead_band = true;  // no answer: on as before (in the dead band or not)
        _suspects  = 0;
        coast(dt);
        return;
    }
    if (raw < _config.rawMin || raw > _config.rawMax) {
        if (!_in_band) {
            _in_band  = true;
            _velocity *= _config.bandRateScale;  // the same turn rate, more of the readings' yaw
        }
        _dead_band = true;
        _suspects  = 0;
        coast(dt);
        return;
    }
    if (_in_band) {
        _in_band  = false;
        _velocity /= _config.bandRateScale;
    }
    // The reading stands for one yaw per turn: take the one closest to where the head should be by now
    float predicted          = 0.0f;
    float predicted_velocity = 0.0f;
    predict(dt, predicted, predicted_velocity);
    const float reading = static_cast<float>(raw - _config.zeroRaw) * _config.yawPerRaw;
    const float yaw     = reading + std::round((predicted - reading) / 3600.0f) * 3600.0f;
    if (std::fabs(yaw - predicted) <= _config.glitchMargin + _config.rateSlack * std::fabs(predicted - _yaw)) {
        _velocity = predicted_velocity;  // the command speeds the head up or slows it down, the readings correct that
        take(nowMs, yaw, raw);
        return;
    }

    // Not where the head can be: a glitch, or the estimate went off. Readings that agree with each other
    // confirmReadings times in a row are right, as long as they make sense: reachable from the last reading taken,
    // and close by while the head rests (it does not move by itself; some glitch values repeat while it rests at the
    // edge of the dead band, far off)
    if (_command == 0 && std::fabs(yaw - _yaw) > _config.restResyncYaw) {
        _suspects  = 0;
        _dead_band = true;
        coast(dt);
        return;
    }
    float suspect = yaw;
    if (_suspects > 0) {
        const float since  = static_cast<float>(elapsed(nowMs, _suspect_ms)) / 1000.0f;
        const float nearer = reading + std::round((_suspect_yaw - reading) / 3600.0f) * 3600.0f;
        if (std::fabs(nearer - _suspect_yaw) <= _config.maxRate * since + _config.glitchMargin) {
            suspect = nearer;
            _suspects++;
        } else {
            _suspects = 0;
        }
    }
    if (_suspects == 0) {
        _suspects         = 1;
        _first_suspect    = suspect;
        _first_suspect_ms = nowMs;
    }
    _suspect_yaw = suspect;
    _suspect_ms  = nowMs;
    if (_suspects >= _config.confirmReadings) {
        const uint32_t span_ms  = elapsed(nowMs, _first_suspect_ms);
        const float span        = static_cast<float>(span_ms) / 1000.0f;
        const float chain_rate  = span > 0.0f ? (suspect - _first_suspect) / span : 0.0f;
        const float commanded   = static_cast<float>(_command) * _config.yawRatePerUnit;
        const float since_taken = static_cast<float>(elapsed(nowMs, _taken_ms)) / 1000.0f;
        const bool reachable    = std::fabs(suspect - _taken_yaw) <= _config.maxRate * since_taken + _config.glitchMargin;
        const bool as_told      = _command != 0 && span_ms >= _config.confirmMoveMs &&
                             std::fabs(chain_rate - commanded) <= _config.confirmRateSlack * std::fabs(commanded);
        const bool resting      = _command == 0 && span_ms >= _config.confirmRestMs;
        if (reachable && (as_told || resting || span_ms >= _config.confirmHoldMs)) {
            _velocity   = chain_rate;
            _has_anchor = false;
            take(nowMs, suspect, raw);
            return;
        }
    }
    _dead_band = true;
    coast(dt);
}

int SpinTracker::yaw() const
{
    return static_cast<int>(std::lround(_yaw));
}

float SpinTracker::velocity() const
{
    return _velocity;
}

bool SpinTracker::inDeadBand() const
{
    return _dead_band;
}

int SpinTracker::takenYaw() const
{
    return static_cast<int>(std::lround(_taken_yaw));
}

bool SpinTracker::clearOfDeadBand() const
{
    return !_dead_band && _last_raw >= _config.rawMin + _config.edgeMargin &&
           _last_raw <= _config.rawMax - _config.edgeMargin;
}

/* -------------------------------------------------------------------------- */
/*                               SpinStallWatch                               */
/* -------------------------------------------------------------------------- */
SpinStallWatch::SpinStallWatch(const Config& config) : _config(config)
{
}

void SpinStallWatch::reset(uint32_t nowMs, int takenYaw)
{
    _ref_yaw    = takenYaw;
    _ref_ms     = nowMs;
    _reading_ms = nowMs;
}

bool SpinStallWatch::update(uint32_t nowMs, int velocity, int yawWay, const SpinTracker& tracker)
{
    if (velocity == 0) {
        reset(nowMs, tracker.takenYaw());  // resting on purpose
        return false;
    }
    if (!tracker.inDeadBand()) {
        _reading_ms = nowMs;
    }
    if ((tracker.takenYaw() - _ref_yaw) * yawWay >= _config.minTurn) {
        _ref_yaw = tracker.takenYaw();
        _ref_ms  = nowMs;
    }
    return elapsed(nowMs, _ref_ms) >= _config.windowMs || elapsed(nowMs, _reading_ms) >= _config.windowMs;
}

}  // namespace stackpet::radar
