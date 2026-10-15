/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "remote_actions.h"
#include <hal/hal.h>
#include <mooncake_log.h>
#include <smooth_lvgl.hpp>
#include <stackchan/stackchan.h>
#include <stackpet/sound/sound_player.h>
#include <algorithm>
#include <cmath>
#include <memory>

namespace stackpet::remote {

using stackchan::avatar::Emotion;

namespace {

constexpr const char* kTag = "RemoteActs";

// Shake / nod: head offsets (0.1 degree) one after the other, kKeyframeMs apart
constexpr int kShakeOffsets[]     = {180, -180, 180, -180, 0};
constexpr int kNodOffsets[]       = {150, 0, 150, 0};
constexpr uint32_t kKeyframeMs    = 160;
constexpr int kActSpeed           = 900;
constexpr uint32_t kMoodMs        = 1600;  // the act's face and words
// Spin: the RADAR's numbers (radar_logic.h, app_radar.cpp); one turn ends where it started, inside the servo's range
constexpr int kSpinVelocity       = 600;
constexpr int kSpinVelocityPerYaw = -1;  // rotate(-v) turns towards +yaw
constexpr float kRatePerUnit      = 4.6f;
constexpr float kCoastMs          = 55.0f;  // PWM 0 freewheels on about this long's worth
// The head followed the stick until the circle was seen: its spring needs ~0.9 s (50 Hz loop) to ~1.4 s (33 Hz) to
// come to rest, and a position write while it moves ends the wheel mode right away. Not at rest by then: no spin.
constexpr uint32_t kRestWaitMs    = 3000;
constexpr uint32_t kTurnTimeoutMs = 3000;
constexpr uint32_t kCoastWaitMs   = 800;
// Leaving the wheel mode, as the RADAR does (app_radar.cpp leave_spin()): position mode at speed would brake hard
// against the gears, so coast down below kStopRate first (at most kStopCoastMs), then off the potentiometer's dead
// band at kEscapeVelocity, the shorter way back to the start (at most kEscapeMs: a head held, or the sensor failed)
constexpr float kStopRate         = 600.0f;  // 0.1 degree/s
constexpr uint32_t kStopCoastMs   = 300;
constexpr int kEscapeVelocity     = 300;
constexpr uint32_t kEscapeMs      = 1500;
constexpr uint32_t kPollMs        = 50;  // isMoving() asks the servo over UART

// DoubleClick: the faces it goes through
constexpr Emotion kFaces[] = {Emotion::Happy,  Emotion::Laughing, Emotion::Surprised, Emotion::Shy,
                              Emotion::Love,   Emotion::Angry,    Emotion::Sad,       Emotion::Doubt,
                              Emotion::Sleepy, Emotion::Neutral};

// The clock only goes forward: unsigned, so a wrap (49 days) does not matter
uint32_t elapsed(uint32_t now, uint32_t since)
{
    return now - since;
}

void say(const char* words, Emotion emotion)
{
    auto& avatar = GetStackChan().avatar();
    avatar.clearDecorators();
    avatar.setEmotion(emotion);
    avatar.setSpeech(words);
}

}  // namespace

void RemoteActions::start(const GestureEvent& event, uint32_t nowMs, int yaw, int pitch)
{
    if (busy() && event.gesture != Gesture::DoubleClick) {
        return;  // a face can change meanwhile, the head is the act's
    }
    switch (event.gesture) {
        case Gesture::Shake:
            say("不要～", Emotion::Doubt);
            SoundPlayer::instance().playSfx(Sfx::Boop);
            begin_act(Act::Shake, nowMs, yaw, pitch);
            break;
        case Gesture::Nod:
            say("好的！", Emotion::Happy);
            SoundPlayer::instance().playSfx(Sfx::Success);
            begin_act(Act::Nod, nowMs, yaw, pitch);
            break;
        case Gesture::Circle:
            say("转圈圈～", Emotion::Laughing);
            SoundPlayer::instance().playSfx(Sfx::Start);
            _spin_direction = event.direction >= 0 ? 1 : -1;
            begin_act(Act::Spin, nowMs, yaw, pitch);
            break;
        case Gesture::DoubleClick: {
            _face       = kFaces[_face_index];
            _face_index = (_face_index + 1) % static_cast<int>(sizeof(kFaces) / sizeof(kFaces[0]));
            if (!busy()) {
                auto& avatar = GetStackChan().avatar();
                avatar.clearSpeech();
                avatar.clearDecorators();
                avatar.setEmotion(_face);
                _mood_on = false;  // stays on, the face between acts from now on
            }  // else on when the act's mood is over
            SoundPlayer::instance().playSfx(Sfx::Click);
            break;
        }
    }
}

void RemoteActions::announce(uint32_t nowMs, const char* words, Emotion emotion)
{
    say(words, emotion);
    _mood_on    = true;
    _mood_from  = nowMs;
    _mood_len   = kMoodMs;
}

void RemoteActions::begin_act(Act act, uint32_t nowMs, int yaw, int pitch)
{
    _act         = act;
    _act_ms      = nowMs;
    _stick_yaw   = yaw;
    _stick_pitch = pitch;
    _keyframe    = 0;
    _mood_on     = true;
    _mood_from   = nowMs;
    _mood_len    = kMoodMs;
    if (act == Act::Spin) {
        _spin_stage = SpinStage::WaitRest;
        _stage_ms   = nowMs;
        _poll_ms    = nowMs - kPollMs;  // the first look right away
        // The yaw spring still chasing the stick: a stiff one to where it is now comes to rest sooner (gate's model:
        // ~40% sooner, and at all down to a 13 Hz main loop)
        auto& yaw_servo = GetStackChan().motion().yawServo();
        yaw_servo.moveWithSpeed(yaw_servo.getCurrentAngle(), 1000);
    }
}

bool RemoteActions::busy() const
{
    return _act != Act::None;
}

void RemoteActions::update(uint32_t nowMs, int stickYaw, int stickPitch)
{
    _stick_yaw   = stickYaw;
    _stick_pitch = stickPitch;
    if (_act == Act::Shake || _act == Act::Nod) {
        update_keyframes(nowMs);
    } else if (_act == Act::Spin) {
        update_spin(nowMs);
    }
    if (_mood_on && elapsed(nowMs, _mood_from) >= _mood_len && _act == Act::None) {
        _mood_on     = false;
        auto& avatar = GetStackChan().avatar();
        avatar.clearSpeech();
        avatar.clearDecorators();
        avatar.setEmotion(_face);
    }
}

void RemoteActions::update_keyframes(uint32_t nowMs)
{
    const int* offsets = _act == Act::Shake ? kShakeOffsets : kNodOffsets;
    const int count    = _act == Act::Shake ? static_cast<int>(sizeof(kShakeOffsets) / sizeof(int))
                                            : static_cast<int>(sizeof(kNodOffsets) / sizeof(int));
    const int due      = static_cast<int>(elapsed(nowMs, _act_ms) / kKeyframeMs);
    while (_keyframe <= due && _keyframe < count) {
        // Around where the stick is (let go after the gesture, the middle)
        int yaw   = _stick_yaw;
        int pitch = _stick_pitch;
        if (_act == Act::Shake) {
            yaw = std::clamp(_stick_yaw + offsets[_keyframe], -1280, 1280);
        } else {
            // Bob towards the middle of the pitch range (it ends at 0 and 900)
            const int bob = _stick_pitch < 450 ? offsets[_keyframe] : -offsets[_keyframe];
            pitch         = std::clamp(_stick_pitch + bob, 0, 900);
        }
        GetStackChan().motion().moveWithSpeed(yaw, pitch, kActSpeed);
        _keyframe++;
    }
    if (_keyframe >= count && elapsed(nowMs, _act_ms) >= static_cast<uint32_t>(count) * kKeyframeMs) {
        end_act(nowMs);
    }
}

void RemoteActions::update_spin(uint32_t nowMs)
{
    auto& motion = GetStackChan().motion();
    auto& yaw    = motion.yawServo();
    switch (_spin_stage) {
        case SpinStage::WaitRest: {
            // A position move still running would end the wheel mode (its next write)
            if (elapsed(nowMs, _poll_ms) < kPollMs) {
                return;
            }
            _poll_ms = nowMs;
            if (yaw.isMoving()) {
                if (elapsed(nowMs, _stage_ms) < kRestWaitMs) {
                    return;
                }
                // Still moving (the stick or the servo): a rotate() now would be undone by the next position write
                mclog::tagWarn(kTag, "spin: the head did not come to rest, no spin");
                say("转不动～", Emotion::Sad);
                SoundPlayer::instance().playSfx(Sfx::Boop);
                _mood_on   = true;
                _mood_from = nowMs;
                _mood_len  = kMoodMs;
                end_act(nowMs);
                return;
            }
            auto config      = _tracker.config();
            const int zero   = yaw.getZeroRawPosition();
            if (zero >= config.rawMin && zero <= config.rawMax) {
                config.zeroRaw = zero;
                _tracker.setConfig(config);
            }
            const int raw   = yaw.readRawPosition();
            _spin_start_yaw = raw >= config.rawMin && raw <= config.rawMax
                                  ? static_cast<int>(std::lround((raw - config.zeroRaw) * config.yawPerRaw))
                                  : motion.getCurrentYawAngle();
            _tracker.reset(nowMs, _spin_start_yaw);
            motion.setAutoTorqueReleaseEnabled(false);  // it would stop the rotation
            const int velocity = _spin_direction * kSpinVelocityPerYaw * kSpinVelocity;
            yaw.rotate(velocity);
            _tracker.setCommand(velocity);
            _spinning   = true;
            _spin_stage = SpinStage::Turning;
            _stage_ms   = nowMs;
            mclog::tagInfo(kTag, "spin from yaw {}", _spin_start_yaw);
            break;
        }
        case SpinStage::Turning: {
            _tracker.update(nowMs, yaw.readRawPosition());
            const float coast  = kSpinVelocity * kRatePerUnit * kCoastMs / 1000.0f;
            const int traveled = (_tracker.yaw() - _spin_start_yaw) * _spin_direction;
            if (static_cast<float>(traveled) >= 3600.0f - coast || elapsed(nowMs, _stage_ms) >= kTurnTimeoutMs) {
                yaw.rotate(0);
                _tracker.setCommand(0);
                _spin_stage = SpinStage::Coasting;
                _stage_ms   = nowMs;
                auto& avatar = GetStackChan().avatar();
                avatar.clearDecorators();
                avatar.setEmotion(Emotion::Doubt);
                avatar.setSpeech("晕了……");
                avatar.addDecorator(
                    std::make_unique<stackchan::avatar::DizzyDecorator>(lv_screen_active(), kMoodMs, 300));
                _mood_on    = true;
                _mood_from  = nowMs;
                _mood_len   = kMoodMs;
            }
            break;
        }
        case SpinStage::Coasting:
            _tracker.update(nowMs, yaw.readRawPosition());
            if ((std::fabs(_tracker.velocity()) < 300.0f && _tracker.clearOfDeadBand()) ||
                elapsed(nowMs, _stage_ms) >= kCoastWaitMs) {
                leave_spin();  // at rest off the dead band by now, as a rule: no wait
                end_act(nowMs);
            }
            break;
    }
}

void RemoteActions::end_act(uint32_t nowMs)
{
    // Where the stick is by now (let go during the act, it is back in the middle; the remote sends nothing more)
    GetStackChan().motion().moveWithSpeed(_stick_yaw, _stick_pitch, 400);
    _act = Act::None;
    if (!_mood_on) {
        _mood_on    = true;
        _mood_from  = nowMs;
        _mood_len   = 400;
    }
}

void RemoteActions::stop()
{
    _act = Act::None;
    leave_spin();
}

void RemoteActions::leave_spin()
{
    if (!_spinning) {
        return;
    }
    auto& motion = GetStackChan().motion();
    auto& yaw    = motion.yawServo();
    // Coast down
    yaw.rotate(0);
    _tracker.setCommand(0);
    uint32_t from = GetHAL().millis();
    while (elapsed(GetHAL().millis(), from) < kStopCoastMs) {
        _tracker.update(GetHAL().millis(), yaw.readRawPosition());
        if (std::fabs(_tracker.velocity()) < kStopRate) {
            break;
        }
        GetHAL().delay(10);
    }
    // Off the dead band, the shorter way back to where the spin started (it started off it)
    if (!_tracker.clearOfDeadBand()) {
        const int to_start = (((_spin_start_yaw - _tracker.yaw()) % 3600) + 5400) % 3600 - 1800;
        const int velocity = (to_start >= 0 ? 1 : -1) * kSpinVelocityPerYaw * kEscapeVelocity;
        yaw.rotate(velocity);
        _tracker.setCommand(velocity);
        from = GetHAL().millis();
        while (!_tracker.clearOfDeadBand() && elapsed(GetHAL().millis(), from) < kEscapeMs) {
            GetHAL().delay(10);
            _tracker.update(GetHAL().millis(), yaw.readRawPosition());
        }
        if (!_tracker.clearOfDeadBand()) {
            mclog::tagWarn(kTag, "spin: the head did not leave the yaw sensor's dead band");
        }
    }
    // Synchronous: PWM 0, back to position mode holding where it is
    yaw.stopRotation();
    motion.setAutoTorqueReleaseEnabled(true);
    _spinning = false;
}

}  // namespace stackpet::remote
