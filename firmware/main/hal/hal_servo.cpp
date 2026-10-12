/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "drivers/FTServo_Arduino/src/SCSCL.h"
#include <stackchan/stackchan.h>
#include <smooth_ui_toolkit.hpp>
#include <mooncake_log.h>
#include <settings.h>
#include <esp_system.h>

using namespace smooth_ui_toolkit;
using namespace stackchan::motion;

static SCSCL _scs_bus;
static bool _scs_bus_ready = false;

static constexpr int kYawServoId = 1;

static void scs_bus_begin()
{
    if (!_scs_bus_ready) {
        _scs_bus_ready = _scs_bus.begin(UART_NUM_1, 1000000, 6, 7);
    }
}

struct ServoConfig_t {
    int id             = -1;
    int defaultZeroPos = 0;
    Vector2i angleLimit;
    Vector2i rawPosLimit;
    std::string settingNs;
    std::string settingZeroPositionKey;
    bool enablePwmMode = false;
    bool enableStallProtection = false;
};

class ScsServo : public Servo {
public:
    static inline const std::string _tag = "ScsServo";

    ScsServo(const ServoConfig_t& config) : _config(config), _runtime_raw_pos_limit(config.rawPosLimit)
    {
    }

    void init() override
    {
        if (_config.enablePwmMode) {
            repair_wheel_mode_leftover();
        }
        reset_runtime_limits();
        set_angle_limit(_config.angleLimit);
        get_zero_pos_from_nvs();
        Servo::init();
    }

    // Wheel (PWM) mode is entered by writing 0/0 to the angle limit registers. The servo keeps them across an
    // ESP32 restart (its power is not cycled), while the cached original limits lived in SCSCL's RAM only. If the
    // previous run ended while rotating (crash, watchdog, reboot), restore position mode here, otherwise position
    // commands are silently ignored until the robot is power cycled.
    void repair_wheel_mode_leftover()
    {
        int min_limit = _scs_bus.readWord(_config.id, SCSCL_MIN_ANGLE_LIMIT_L);
        int max_limit = _scs_bus.readWord(_config.id, SCSCL_MAX_ANGLE_LIMIT_L);
        mclog::tagInfo(_tag, "id: {} angle limit registers: {} / {}", _config.id, min_limit, max_limit);
        if (min_limit < 0 || max_limit < 0) {
            return;  // servo not answering, nothing to repair
        }

        // Remember this unit's real limits (they differ per servo, e.g. 20 / 1003) for a later repair. Only a
        // proper range counts: 0 / 0 is wheel mode, and a half restored pair (e.g. 20 / 0) is broken too.
        std::string min_key = fmt::format("lim_min_{}", _config.id);
        std::string max_key = fmt::format("lim_max_{}", _config.id);
        if (min_limit < max_limit) {
            Settings settings(_config.settingNs, true);
            if (settings.GetInt(min_key, -1) != min_limit || settings.GetInt(max_key, -1) != max_limit) {
                settings.SetInt(min_key, min_limit);
                settings.SetInt(max_key, max_limit);
            }
            _scs_bus.CacheAngleLimit(_config.id, min_limit, max_limit);
            return;
        }

        int restore_min = kFallbackMinLimit;
        int restore_max = kFallbackMaxLimit;
        {
            Settings settings(_config.settingNs, false);
            int saved_min = settings.GetInt(min_key, -1);
            int saved_max = settings.GetInt(max_key, -1);
            if (saved_min >= 0 && saved_max > saved_min) {
                restore_min = saved_min;
                restore_max = saved_max;
            }
        }
        // Torque off first: once the limits are back the servo would otherwise rush to the stale goal position
        _scs_bus.EnableTorque(_config.id, 0);
        _scs_bus.WritePWM(_config.id, 0);
        _scs_bus.writeWord(_config.id, SCSCL_MIN_ANGLE_LIMIT_L, static_cast<uint16_t>(restore_min));
        _scs_bus.writeWord(_config.id, SCSCL_MAX_ANGLE_LIMIT_L, static_cast<uint16_t>(restore_max));
        _scs_bus.CacheAngleLimit(_config.id, restore_min, restore_max);
        mclog::tagWarn(_tag, "id: {} angle limits were {} / {} (wheel mode leftover), restored {} / {}", _config.id,
                       min_limit, max_limit, restore_min, restore_max);
    }

    void get_zero_pos_from_nvs()
    {
        _zero_pos     = _config.defaultZeroPos;
        bool is_valid = false;

        {
            Settings settings(_config.settingNs, false);
            int nvs_zero_pos = settings.GetInt(_config.settingZeroPositionKey, -1);

            // Limit check
            if (nvs_zero_pos >= _config.rawPosLimit.x && nvs_zero_pos <= _config.rawPosLimit.y) {
                _zero_pos = nvs_zero_pos;
                is_valid  = true;
                mclog::tagInfo(_tag, "id: {} get zero pos: {} from settings", _config.id, _zero_pos);
            } else {
                is_valid = false;
                mclog::tagWarn(_tag, "id: {} get invalid zero pos: {} from settings", _config.id, nvs_zero_pos);
            }
        }

        if (!is_valid) {
            _zero_pos = _config.defaultZeroPos;
            mclog::tagInfo(_tag, "id: {} override zero pos to default: {}", _config.id, _zero_pos);

            Settings settings(_config.settingNs, true);
            settings.SetInt(_config.settingZeroPositionKey, _zero_pos);
        }
    }

    void set_angle_impl(int angle) override
    {
        int mapped_angle = _zero_pos + angle * 16 / 5 / 10;  // 一步对应 0.3125度, 0.3125 = 5/16
        mapped_angle     = uitk::clamp(mapped_angle, _runtime_raw_pos_limit.x, _runtime_raw_pos_limit.y);

        // mclog::tagInfo(_tag, "id: {} mapped angle: {}", _id, mapped_angle);

        if (update_stall_protection(mapped_angle)) {
            return;
        }

        check_mode(Mode::Position);
        _scs_bus.WritePos(_config.id, mapped_angle, 20, 0);
    }

    int readRawPosition() override
    {
        return _scs_bus.ReadPos(_config.id);
    }

    int readTemperature() override
    {
        return _scs_bus.ReadTemper(_config.id);
    }

    int getZeroRawPosition() override
    {
        return _zero_pos;
    }

    int getCurrentAngle() override
    {
        int current_pos = _scs_bus.ReadPos(_config.id);
        if (!is_raw_pos_valid(current_pos)) {
            int fallback_angle = uitk::clamp(Servo::getCurrentAngle(), getAngleLimit().x, getAngleLimit().y);
            mclog::tagWarn(_tag, "id: {} ignore invalid current pos: {}, fallback angle: {}", _config.id, current_pos,
                           fallback_angle);
            return fallback_angle;
        }

        int angle = raw_pos_to_angle(current_pos);
        angle     = uitk::clamp(angle, getAngleLimit().x, getAngleLimit().y);
        // mclog::tagInfo(_tag, "id: {} current pos: {} angle: {}", _id, current_pos, angle);
        return angle;
    }

    bool is_moving_impl() override
    {
        int moving = _scs_bus.ReadMove(_config.id);
        // mclog::tagInfo(_tag, "id: {} moving: {}", _id, moving);
        return moving != 0;
    }

    void setTorqueEnabled(bool enabled) override
    {
        Servo::setTorqueEnabled(enabled);
        _scs_bus.EnableTorque(_config.id, enabled ? 1 : 0);
        // mclog::tagInfo(_tag, "id: {} set torque: {}", _id, enabled);
    }

    bool getTorqueEnabled() override
    {
        int torque_enable = _scs_bus.ReadToqueEnable(_config.id);
        // mclog::tagInfo(_tag, "id: {} torque enable: {}", _id, torque_enable);
        return torque_enable > 0;
    }

    void setCurrentAngleAsZero() override
    {
        int current_pos = _scs_bus.ReadPos(_config.id);
        if (!is_raw_pos_valid(current_pos)) {
            mclog::tagWarn(_tag, "id: {} ignore invalid zero calibration pos: {}, keep zero pos: {}", _config.id,
                           current_pos, _zero_pos);
            return;
        }

        _zero_pos = current_pos;
        reset_runtime_limits();

        Settings settings(_config.settingNs, true);
        settings.SetInt(_config.settingZeroPositionKey, _zero_pos);

        mclog::tagInfo(_tag, "id: {} set zero pos: {} to settings", _config.id, _zero_pos);
    }

    void resetZeroCalibration() override
    {
        _zero_pos = _config.defaultZeroPos;
        reset_runtime_limits();

        Settings settings(_config.settingNs, true);
        settings.SetInt(_config.settingZeroPositionKey, _zero_pos);

        mclog::tagInfo(_tag, "id: {} set zero pos: {} to settings", _config.id, _zero_pos);
    }

    void rotate(int velocity) override
    {
        velocity = uitk::clamp(velocity, -1000, 1000);

        if (!_config.enablePwmMode) {
            return;
        }

        if (_current_mode != Mode::PWM) {
            begin_rotation();
        }
        int mapped_velocity = map_range(velocity, 0, 1000, 0, 1023);

        check_mode(Mode::PWM);
        _scs_bus.WritePWM(_config.id, mapped_velocity);
    }

    void stopRotation() override
    {
        if (_current_mode != Mode::PWM) {
            return;
        }
        // Set the goal to where the head is *before* restoring the angle limits: in wheel mode this WritePos also
        // writes GOAL_TIME (= PWM) 0, so the motor stops, and when position mode comes back the servo holds still
        // instead of rushing to the goal left over from before the rotation. Two readings that agree: next to the
        // sensor's dead band a single one can be far off (e.g. 221 right after it).
        int raw_pos         = _scs_bus.ReadPos(_config.id);
        const int raw_again = _scs_bus.ReadPos(_config.id);
        bool pos_valid      = is_raw_pos_valid(raw_pos) && is_raw_pos_valid(raw_again) &&
                         abs_int(raw_again - raw_pos) <= kStopReadAgreeRaw;
        if (pos_valid) {
            raw_pos = raw_again;
            _scs_bus.WritePos(_config.id, raw_pos, 0, 0);
        } else {
            // No position to hold (the dead band): torque off first, or the servo would rush to that old goal once
            // position mode is back. The next position command turns it on again.
            setTorqueEnabled(false);
            _scs_bus.WritePWM(_config.id, 0);
        }
        check_mode(Mode::Position);
        if (pos_valid) {
            stop_motion_at_angle(uitk::clamp(raw_pos_to_angle(raw_pos), getAngleLimit().x, getAngleLimit().y));
        }
        mclog::tagInfo(_tag, "id: {} rotation stopped, {} position mode", _config.id,
                       _current_mode == Mode::Position ? "back to" : "FAILED to restore");
    }

private:
    enum class Mode { Position = 0, PWM = 1 };

    // Used only when a wheel mode leftover must be repaired before this unit's limits were ever saved
    // (the yaw servo of the StackChan this was developed on reports 20 / 1003)
    static constexpr int kFallbackMinLimit = 20;
    static constexpr int kFallbackMaxLimit = 1003;

    ServoConfig_t _config;
    Vector2i _runtime_raw_pos_limit;
    int _zero_pos      = 0;
    Mode _current_mode = Mode::Position;

    static constexpr int kStopReadAgreeRaw             = 8;  // stopRotation(): two readings this close
    static constexpr uint32_t kStallFeedbackIntervalMs = 50;
    static constexpr int kStallMinTargetDeltaRaw       = 8;
    static constexpr int kStallMaxPositionDeltaRaw     = 1;
    static constexpr int kStallCurrentRiseThreshold    = 80;
    static constexpr int kStallLoadRiseThreshold       = 150;
    static constexpr int kStallCurrentAbsThreshold     = 350;
    static constexpr int kStallLoadAbsThreshold        = 650;
    static constexpr int kStallConfirmSamples          = 2;

    uint32_t _last_stall_check_tick = 0;
    int _last_stall_raw_pos         = 0;
    int _last_stall_current_abs     = 0;
    int _last_stall_load_abs        = 0;
    int _last_stall_direction       = 0;
    int _stall_confirm_count        = 0;
    bool _last_stall_feedback_valid = false;

    static int abs_int(int value)
    {
        return value < 0 ? -value : value;
    }

    bool is_raw_pos_valid(int raw_pos) const
    {
        return raw_pos >= _config.rawPosLimit.x && raw_pos <= _config.rawPosLimit.y;
    }

    int raw_pos_to_angle(int raw_pos) const
    {
        return (raw_pos - _zero_pos) * 5 * 10 / 16;
    }

    void reset_runtime_limits()
    {
        _runtime_raw_pos_limit = _config.rawPosLimit;
        set_angle_limit(_config.angleLimit);
        reset_stall_detection();
    }

    void reset_stall_detection()
    {
        _last_stall_feedback_valid = false;
        _last_stall_direction      = 0;
        _stall_confirm_count       = 0;
    }

    bool update_stall_protection(int target_raw_pos)
    {
        if (!_config.enableStallProtection) {
            return false;
        }

        const uint32_t now = GetHAL().millis();
        if (now - _last_stall_check_tick < kStallFeedbackIntervalMs) {
            return false;
        }
        _last_stall_check_tick = now;

        if (_scs_bus.FeedBack(_config.id) < 0) {
            reset_stall_detection();
            return false;
        }

        const int current_raw_pos = _scs_bus.ReadPos(-1);
        const int current_abs     = abs_int(_scs_bus.ReadCurrent(-1));
        const int load_abs        = abs_int(_scs_bus.ReadLoad(-1));

        if (!is_raw_pos_valid(current_raw_pos)) {
            reset_stall_detection();
            return false;
        }

        const int target_delta = target_raw_pos - current_raw_pos;
        if (abs_int(target_delta) < kStallMinTargetDeltaRaw) {
            reset_stall_detection();
            return false;
        }

        const int direction = target_delta > 0 ? 1 : -1;
        if (_last_stall_feedback_valid && direction == _last_stall_direction) {
            const int pos_delta = abs_int(current_raw_pos - _last_stall_raw_pos);
            const bool position_stuck = pos_delta <= kStallMaxPositionDeltaRaw;
            const bool current_spike  = current_abs >= kStallCurrentAbsThreshold ||
                                       current_abs - _last_stall_current_abs >= kStallCurrentRiseThreshold;
            const bool load_spike = load_abs >= kStallLoadAbsThreshold ||
                                    load_abs - _last_stall_load_abs >= kStallLoadRiseThreshold;

            if (position_stuck && (current_spike || load_spike)) {
                _stall_confirm_count++;
            } else if (pos_delta > kStallMaxPositionDeltaRaw) {
                _stall_confirm_count = 0; 
            }
        } else {
            _stall_confirm_count = 0;
        }

        _last_stall_raw_pos         = current_raw_pos;
        _last_stall_current_abs     = current_abs;
        _last_stall_load_abs        = load_abs;
        _last_stall_direction       = direction;
        _last_stall_feedback_valid = true;

        if (_stall_confirm_count < kStallConfirmSamples) {
            return false;
        }

        handle_stall(current_raw_pos, direction, current_abs, load_abs);
        return true;
    }

    void handle_stall(int raw_pos, int direction, int current_abs, int load_abs)
    {
        int angle = raw_pos_to_angle(raw_pos);
        angle     = uitk::clamp(angle, _config.angleLimit.x, _config.angleLimit.y);

        auto angle_limit = getAngleLimit();
        if (direction > 0) {
            if (raw_pos < _runtime_raw_pos_limit.y) {
                _runtime_raw_pos_limit.y = raw_pos;
            }
            if (angle < angle_limit.y) {
                angle_limit.y = angle;
            }
        } else {
            if (raw_pos > _runtime_raw_pos_limit.x) {
                _runtime_raw_pos_limit.x = raw_pos;
            }
            if (angle > angle_limit.x) {
                angle_limit.x = angle;
            }
        }
        set_angle_limit(angle_limit);
        stop_motion_at_angle(angle);
        reset_stall_detection();

        check_mode(Mode::Position);
        _scs_bus.WritePos(_config.id, raw_pos, 20, 0);

        mclog::tagWarn(_tag,
                       "id: {} stall detected, raw: {}, angle: {}, dir: {}, current: {}, load: {}, limit: [{}, {}]",
                       _config.id, raw_pos, angle, direction, current_abs, load_abs, angle_limit.x, angle_limit.y);
    }

    void check_mode(Mode targetMode)
    {
        if (targetMode == _current_mode) {
            return;
        }

        int ret = _scs_bus.SwitchMode(_config.id, static_cast<uint8_t>(targetMode));
        // SCSCL conventions: entering wheel mode returns the write ack (1), restoring the limits returns 0
        bool ok = targetMode == Mode::PWM ? ret == 1 : ret == 0;
        if (!ok) {
            mclog::tagWarn(_tag, "id: {} switch to {} mode failed: {}", _config.id,
                           targetMode == Mode::PWM ? "wheel" : "position", ret);
            return;  // keep the old mode so the switch is retried by the next command
        }
        _current_mode = targetMode;
    }
};

void Hal::servo_early_stop()
{
    // A warm restart does not cut the servo power: if the previous run died while the yaw servo was spinning
    // (panic, watchdog, shutdown handlers not run), stop it now instead of after the display/camera init
    scs_bus_begin();
    if (_scs_bus_ready) {
        _scs_bus.WritePWM(kYawServoId, 0);
    }
}

void Hal::servo_init()
{
    mclog::tagInfo("HAL-Servo", "init");

    scs_bus_begin();

    ServoConfig_t yaw_servo_config;
    yaw_servo_config.id                     = kYawServoId;
    yaw_servo_config.defaultZeroPos         = 460;
    yaw_servo_config.angleLimit             = Vector2i(-1280, 1280);
    yaw_servo_config.rawPosLimit            = Vector2i(0, 1000);
    yaw_servo_config.settingNs              = "servo";
    yaw_servo_config.settingZeroPositionKey = "zero_pos_1";
    yaw_servo_config.enablePwmMode          = true;

    ServoConfig_t pitch_servo_config;
    pitch_servo_config.id                     = 2;
    pitch_servo_config.defaultZeroPos         = 620;
    pitch_servo_config.angleLimit             = Vector2i(30, 870);
    pitch_servo_config.rawPosLimit            = Vector2i(0, 1000);
    pitch_servo_config.settingNs              = "servo";
    pitch_servo_config.settingZeroPositionKey = "zero_pos_2";
    pitch_servo_config.enableStallProtection  = true;

    auto yaw_servo   = std::make_unique<ScsServo>(yaw_servo_config);
    auto pitch_servo = std::make_unique<ScsServo>(pitch_servo_config);
    auto motion      = std::make_unique<Motion>(std::move(yaw_servo), std::move(pitch_servo));
    motion->init();

    GetStackChan().attachMotion(std::move(motion));

    // Last line of defence for every restart path (warm reboot, panic handler excluded): never leave the yaw
    // servo spinning in wheel mode while the chip restarts
    esp_err_t err = esp_register_shutdown_handler([]() {
        if (GetStackChan().hasMotion()) {
            GetStackChan().motion().yawServo().stopRotation();
        }
    });
    if (err != ESP_OK) {
        mclog::tagError("HAL-Servo", "failed to register the rotation stop shutdown handler: {}", esp_err_to_name(err));
    }
}
