/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_radar.h"
#include "radar_logic.h"
#include "radar_view.h"
#include <hal/hal.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <stackchan/stackchan.h>
#include <apps/common/common.h>
#include <assets/assets.h>
#include <smooth_lvgl.hpp>
#include <stackpet/stackpet.h>
#include <stackpet/camera/frame_grabber.h>
#include <stackpet/face/face.h>
#include <stackpet/fx/head_touch.h>
#include <stackpet/fx/led_fx.h>
#include <stackpet/remote/remote_link.h>
#include <stackpet/sound/sound_player.h>
#include <stackpet/ui/ui.h>
#include <stackpet/vision/face_bench.h>
#include <stackpet/vision/face_detector.h>
#include <stackpet/vision/motion_detector.h>
#include <fmt/format.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <string>
#include <vector>

using namespace mooncake;
using namespace stackchan;
using namespace stackpet;
using namespace stackpet::radar;

static constexpr uint32_t kThemeColor = 0x3DDC84;
static constexpr uint32_t kThemeDark  = 0x103A22;

/* ------------------------------ Calibration ------------------------------- */
// Angles in 0.1 degree. Tune them on the device.
static constexpr int kScanPitch     = 200;   // 20 degree up: faces of people sitting around the desk (34 degree view)
static constexpr int kYawLimit      = 1250;  // sweep +-125 degree
static constexpr int kSweepMaxSpeed = 320;   // 32 degree/s
static constexpr int kSweepMinSpeed = 100;   // a slow detector slows the sweep down to this
static constexpr int kSweepAccel    = 600;   // braking at the ends, 60 degree/s^2
// Measured with a face at about 1 m (host tool aim_cal.py, 2026-10-12): 52 x 34 degree field of view, a face right of
// the image centre is towards +yaw, one below it towards -pitch
static constexpr int kHalfFovYaw    = 261;
static constexpr int kHalfFovPitch  = 171;
static constexpr int kImageXToYaw   = 1;
static constexpr int kImageYToPitch = -1;
static constexpr int kServoLagMs    = 120;   // the head trails the streamed sweep position by about this much
static constexpr int kStreamSpeed   = 900;   // stiff spring for the streamed sweep: the planner shapes the motion
static constexpr int kPitchSpeed    = 300;
static constexpr float kFramesPerView = 6.0f;  // detector runs while a face crosses the field of view
static constexpr float kMinDetectFps  = 0.7f;  // a detector slower than this: switch to the motion fallback
static constexpr float kDarkLuma      = 30.0f;
static constexpr float kMaxRangeM     = 3.0f;  // outer ring of the scope
static constexpr int kGazeRange       = 60;    // eye offset (avatar units) for a target at the image edge

// Spin scan (wheel mode, always the same way round: the USB cable is on the base). Measured: rotate(v) turns at about
// 0.46 degree/s per unit towards -yaw (800: ~370 degree/s, the SPIN game's speed, which the user asked for). Two fast
// turns, then a look-around turn with 8 stops (no face is recognized in a spinning frame, still frames are fine).
static constexpr int kSpinVelocity          = 800;
static constexpr int kSpinVelocityPerYaw    = -1;
static constexpr float kSpinYawRate         = 3700.0f;  // 0.1 degree/s at kSpinVelocity (radar afterglow scale)
static constexpr int kDeadBandEscapeVelocity = 300;     // leaving the sensor's dead band before a position move
static constexpr uint32_t kDeadBandEscapeMs  = 1500;    // ... at most this long (a head held by hand or a cable)
static constexpr float kStopRate            = 600.0f;   // leaving wheel mode: coast down below this (0.1 degree/s) ...
static constexpr uint32_t kCoastMs           = 300;      // ... for at most this long (position mode at speed brakes hard)
static constexpr uint32_t kCloseEscapeMs     = 600;      // closing the app: at most this long to get off the dead band
static constexpr uint32_t kBrakeWaitMs       = 200;      // a look-around stop: at most this long to its brake point
static constexpr int kLookEdgeMargin         = 100;      // look-around stops stay this far (0.1 degree) off the dead band
// Driven, but the head did not turn (SpinStallWatch: 10 degree in 1 s by the readings): rest kStallBackoffMs, try again;
// kMaxStalls in kStallWindowMs: stuck, sweep instead until the head is petted or the app opened again
static constexpr uint32_t kStallBackoffMs    = 3000;
static constexpr int kMaxStalls              = 3;
static constexpr uint32_t kStallWindowMs     = 60000;
static constexpr uint32_t kUnstickDelayMs    = 1000;     // stuck, petted: spin again this long after the hand is off
static constexpr uint32_t kCheckFailedShowMs = 1000;    // "没看清" after a face that could not be confirmed
// Listening for the remote: how to go back shown this long
static constexpr uint32_t kRemoteHintMs     = 15000;
static constexpr int kSpinFrameLatencyMs    = 40;
static constexpr uint32_t kSpinCheckMs      = 1500;
static constexpr uint32_t kSpinStartWaitMs  = 1500;     // at most for the yaw spring to come to rest before spinning
static constexpr uint32_t kTempCheckMs      = 5000;
// The SCS0009 unloads itself at 70 C (its register 13 default): keep clear of that
static constexpr int kServoWarmC            = 55;       // the yaw servo at this: spin slower
static constexpr int kServoHotC             = 65;       // at this: stop spinning until it is below kServoWarmC
static constexpr int kTempFailLimit         = 3;        // that many failed reads in a row (15 s): taken as hot
static constexpr int kSpinWarmVelocity      = 500;
static constexpr uint32_t kLedCheck         = 0xFFB000;

// Motion fallback (no face model): stop every 40 degree for a second and look for movement
static constexpr int kDwellEvery              = 400;
static constexpr uint32_t kDwellMs            = 1000;
static constexpr uint8_t kMotionCellThreshold = 24;
static constexpr int kMotionMinBlob           = 4;  // cells of the 40x30 luma grid
static constexpr float kMotionMinScore        = 0.008f;

/* ---------------------------------- Colors --------------------------------- */
static constexpr uint32_t kLedAlarm  = 0xFF0000;
static constexpr uint32_t kLedAngry  = 0xFF1010;
static constexpr uint32_t kLedCalm   = 0x00B850;
static constexpr uint32_t kLedFriend = 0xFF3C82;
static constexpr uint8_t kScanLedMax = 200;
static constexpr uint32_t kTextGreen  = 0x3DFF7A;
static constexpr uint32_t kTextYellow = 0xFFC400;
static constexpr uint32_t kTextOrange = 0xFF9500;
static constexpr uint32_t kTextRed    = 0xFF453A;
static constexpr uint32_t kTextCyan   = 0x3CE1FF;

static constexpr uint32_t kHintDelayMs = 4500;  // after the lock-on: tell people how to calm the robot down

enum class VisionMode : uint8_t { Face, Motion, Blind };


struct AppRadar::State {
    std::unique_ptr<RadarMachine> machine;
    RadarScreen screen;
    LockView lock_view;
    uitk::lvgl_cpp::Container* avatar_panel = nullptr;
    fx::HeadTouch head_touch;
    fx::LedAnimator leds;
    vision::FaceTracker tracker;
    vision::MotionDetector motion;
    VisionMode mode = VisionMode::Blind;
    Phase shown_phase = Phase::Scanning;
    Phase previous_phase = Phase::Scanning;

    // Vision
    uint32_t face_seq      = 0;
    vision::FaceFrame face_frame;  // latest detector result: its frame is what the lock view shows
    bool face_frame_fresh  = false;
    bool detector_ready    = false;
    float detect_fps       = 0.0f;
    float mean_luma        = -1.0f;
    LumaFrame luma;
    int motion_hits        = 0;
    uint32_t rgb_seq       = 0;
    Contact last_contact;

    // Head
    uint32_t applied_serial = 0;
    bool streaming          = false;
    // Spin scan: yaw servo in wheel mode, its position read every tick
    SpinTracker spin_tracker;
    bool spinning           = false;
    bool spin_waiting       = false;  // asked to spin, waiting for the yaw spring to come to rest
    uint32_t spin_wait_ms   = 0;
    uint32_t spin_poll_ms   = 0;
    int spin_velocity       = 0;
    uint32_t temp_checked   = 0;
    uint32_t temp_logs      = 0;
    bool looking_logged     = false;
    uint32_t check_failed_ms = 0;
    bool check_failed_shown  = false;
    bool escaping            = false;  // leaving the dead band before stopping the spin
    uint32_t escape_from_ms  = 0;
    int escape_velocity      = 0;      // rotate() velocity for it: the shorter way round to the next target
    SpinStallWatch stall_watch;
    uint32_t brake_serial    = 0;      // the look-around brake done last
    std::array<int, 8> looked{};       // where the look-around stops had the head (logged per turn)
    size_t looked_count      = 0;
    bool backing_off         = false;  // resting after a stall until backoff_until
    uint32_t backoff_until   = 0;
    int stalls               = 0;      // in the window from stalls_from_ms
    uint32_t stalls_from_ms  = 0;
    bool spin_stuck          = false;  // stalled too often: sweeping until petted or opened again
    bool unstick_pending     = false;  // petted while stuck: spinning again at unstick_ms
    uint32_t unstick_ms      = 0;
    // The remote link still listening (opened with the remote, or touched open once it has been used): its M5 button
    // goes home (the home indicator asks the link)
    bool remote_session      = false;
    uint32_t remote_since_ms = 0;  // opened at (how to go back is shown for a while)
    int temp_fails           = 0;
    bool servo_paused       = false;  // too hot: spinning at 0 until it cooled down
    int streamed_yaw        = INT_MIN;
    int streamed_pitch      = INT_MIN;
    int sweep_speed         = kSweepMaxSpeed;
    uint32_t speed_checked  = 0;

    // Screen / LEDs
    uint32_t bearing_shown_ms = 0;
    uint32_t led_written_ms   = 0;
    std::array<uint8_t, 6> led_levels{};
    bool led_valid            = false;
    int gaze_x                = 0;
    int gaze_y                = 0;
    bool hint_shown           = false;
};

AppRadar::AppRadar()
{
    setAppInfo().name = "RADAR";
    static auto icon  = assets::get_image("icon_radar.bin");
    setAppInfo().icon = (void*)&icon;
    static uint32_t theme_color = kThemeColor;
    setAppInfo().userData       = (void*)&theme_color;
}

AppRadar::~AppRadar() = default;

void AppRadar::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

void AppRadar::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    _state   = std::make_unique<State>();
    auto& st = *_state;
    st.remote_session = stackpet::remote::RemoteLink::instance().listening();
    if (st.remote_session) {
        st.remote_since_ms = GetHAL().millis();
        mclog::tagInfo(getAppInfo().name, "remote listening: its M5 button goes home");
    }

    RadarConfig config;
    config.sweep.limit   = kYawLimit;
    config.sweep.speed   = kSweepMaxSpeed;
    config.sweep.accel   = kSweepAccel;
    config.scanPitch     = kScanPitch;
    config.halfFovYaw    = kHalfFovYaw;
    config.halfFovPitch  = kHalfFovPitch;
    config.imageXToYaw   = kImageXToYaw;
    config.imageYToPitch = kImageYToPitch;
    config.servoLagMs    = kServoLagMs;
    config.spinScan           = true;  // the motion fallback switches back to the sweep
    config.spinVelocity       = kSpinVelocity;
    config.spinVelocityPerYaw = kSpinVelocityPerYaw;
    config.frameLatencyMs     = kSpinFrameLatencyMs;
    config.checkMs            = kSpinCheckMs;
    st.machine           = std::make_unique<RadarMachine>(config);
    st.motion.setCellThreshold(kMotionCellThreshold);

    // RGB565 frames for the face detector only (converted on demand), no luma unless the fallback needs it
    GrabberConfig grabber_config;
    grabber_config.maxFps       = 15;
    grabber_config.lumaWidth    = 0;
    grabber_config.lumaHeight   = 0;
    grabber_config.rgbDownscale = 1;
    if (FrameGrabber::instance().start(grabber_config)) {
        st.mode = VisionMode::Face;
#if STACKPET_FD_BENCH
        // Test builds only: measure the detector variants instead (the FaceDetector stays off, see onRunning())
        vision::startFaceBench();
#else
        if (!vision::FaceDetector::instance().start()) {
            enter_motion_fallback("the detector task could not be created");
        }
#endif
    } else {
        st.mode = VisionMode::Blind;
        mclog::tagWarn(getAppInfo().name, "camera unavailable: scanning only");
    }

    st.head_touch.attach();

    LvglLockGuard lock;

    auto avatar = stackpet::face::create(lv_screen_active(), ui::fontCn20(), &st.avatar_panel);
    GetStackChan().attachAvatar(std::move(avatar));
    GetStackChan().addModifier(std::make_unique<BlinkModifier>());
    set_avatar_visible(false);

    st.screen.create(lv_screen_active(), ui::fontCn20());
    // The dashed lines: how far round the spin scan can still lock on (behind them: gray blips, no alarm)
    st.screen.setLimitYaw(st.machine->config().aimYawLimit +
                          static_cast<int>(st.machine->config().reachFov * st.machine->config().halfFovYaw));
    st.lock_view.create(lv_screen_active(), ui::fontTitle28(), ui::fontCn20());

    view::create_home_indicator([&]() { close(); }, kThemeColor, kThemeDark);
    view::create_status_bar(kThemeColor, kThemeDark);

    auto& motion  = GetStackChan().motion();
    const int yaw = std::clamp(motion.getCurrentYawAngle(), -kYawLimit, kYawLimit);
    st.machine->start(GetHAL().millis(), yaw, kScanPitch);
    update_status();
}

void AppRadar::onRunning()
{
#if STACKPET_FD_BENCH
    // Test builds only: the head, the speaker and the LEDs stay idle while the bench runs
    {
        LvglLockGuard lock;
        view::update_home_indicator();
    }
    return;
#endif
    auto& st           = *_state;
    const uint32_t now = GetHAL().millis();

    RadarInput input;
    input.nowMs       = now;
    const auto touch  = st.head_touch.poll();
    input.petted      = touch.pressed;
    if (touch.tapped && st.spin_stuck && st.mode == VisionMode::Face) {
        // Freed: spin again a moment after the hand is off the head
        st.unstick_pending = true;
        st.unstick_ms      = now + kUnstickDelayMs;
    }
    if (st.unstick_pending && static_cast<int32_t>(now - st.unstick_ms) >= 0) {
        mclog::tagInfo(getAppInfo().name, "spin: petted, spinning again");
        st.unstick_pending = false;
        st.spin_stuck      = false;
        st.stalls          = 0;
        st.machine->setSpinScan(true, now);
    }
    // Outside the LVGL lock: may stop the detector (blocking) when switching to the fallback
    update_vision(now, input.contact);
    // The head right after its reading, also outside the lock (getting it can take a whole render): a spin brakes
    // where the reading says
    update_spin_inputs(now, input);
    update_spin_safety(now);
    st.machine->update(input);
    apply_head(now);

    LvglLockGuard lock;

    for (const auto& event : st.machine->takeEvents()) {
        handle_event(event, now);
    }
    update_screen(now);
    update_leds(now);

    GetStackChan().update();

    view::update_home_indicator();
    view::update_status_bar();
}

void AppRadar::update_vision(uint32_t now, Contact& contact)
{
    auto& st = *_state;

    if (st.mode == VisionMode::Face) {
        auto& detector   = vision::FaceDetector::instance();
        const auto stats = detector.stats();
        st.detector_ready = stats.ready;
        st.detect_fps     = stats.fps;
        if (stats.failed) {
            enter_motion_fallback("the face model could not be loaded");
            return;
        }
        // Judged on the average inference time: a few runs are enough, even when every run takes seconds
        if (stats.ready && stats.runs >= 3 && stats.inferMs > 1000.0f / kMinDetectFps) {
            enter_motion_fallback("face detection is too slow");
            return;
        }

        vision::FaceFrame frame;
        if (!detector.latest(st.face_seq, frame)) {
            return;
        }
        st.face_seq                = frame.seq;
        st.mean_luma               = frame.meanLuma;
        const uint32_t capture_ms  = static_cast<uint32_t>(frame.frame.timestampUs / 1000);
        const auto& target         = st.tracker.update(frame.faces, capture_ms);
        contact.fresh              = true;
        contact.present            = target.present && target.hits > 0;
        contact.hits               = target.hits;
        contact.x                  = target.raw.cx;
        contact.y                  = target.raw.cy;
        contact.w                  = target.raw.w;
        contact.h                  = target.raw.h;
        contact.captureMs          = capture_ms;
        st.face_frame              = std::move(frame);
        st.face_frame_fresh        = true;
        if (contact.present) {
            st.last_contact = contact;
        }
        return;
    }

    if (st.mode == VisionMode::Motion) {
        if (!FrameGrabber::instance().latestLuma(st.luma.seq, st.luma) || st.luma.pixels.empty()) {
            return;
        }
        st.mean_luma = vision::meanLuma(st.luma.pixels.data(), st.luma.width, st.luma.height);
        // Frame differences only mean something while the head stands still in a dwell
        if (!st.machine->dwellSettled(now)) {
            st.motion.clearReference();
            st.motion_hits = 0;
            return;
        }
        if (!st.motion.hasReference()) {
            st.motion.setReference(st.luma.pixels.data(), st.luma.width, st.luma.height);
            return;
        }
        const auto result = st.motion.compare(st.luma.pixels.data(), st.luma.width, st.luma.height);
        const bool moving = result.valid && result.blobCells >= kMotionMinBlob && result.score >= kMotionMinScore;
        st.motion_hits    = moving ? st.motion_hits + 1 : 0;
        const float side  = std::sqrt(static_cast<float>(std::max(result.blobCells, 1)));
        contact.fresh     = true;
        contact.present   = moving;
        contact.hits      = st.motion_hits;
        contact.x         = result.blobCx;
        contact.y         = result.blobCy;
        contact.w         = side / st.luma.width;
        contact.h         = side / st.luma.height;
        contact.captureMs = static_cast<uint32_t>(st.luma.timestampUs / 1000);
        if (moving) {
            st.last_contact = contact;
        }
    }
}

void AppRadar::enter_motion_fallback(const char* reason)
{
    auto& st = *_state;
    mclog::tagWarn(getAppInfo().name, "face detection off ({}): motion detection fallback", reason);
    vision::FaceDetector::instance().stop();

    GrabberConfig grabber_config;
    grabber_config.maxFps     = 15;
    grabber_config.lumaWidth  = 40;
    grabber_config.lumaHeight = 30;
    FrameGrabber::instance().start(grabber_config);  // a running grabber adopts the new config

    st.mode = VisionMode::Motion;
    st.motion.clearReference();
    st.motion_hits = 0;
    st.tracker.reset();
    st.face_frame       = vision::FaceFrame{};
    st.face_frame_fresh = false;
    st.machine->setSweepSpeed(kSweepMaxSpeed);
    st.machine->setDwellScan(kDwellEvery, kDwellMs);
    // Frame differences need a still camera: the sweep with its dwells instead of the spin
    st.machine->setSpinScan(false, GetHAL().millis());
    {
        LvglLockGuard lock;
        st.screen.setLimitYaw(kYawLimit);
    }
}

void AppRadar::set_avatar_visible(bool visible)
{
    auto& st = *_state;
    if (st.avatar_panel != nullptr) {
        st.avatar_panel->setHidden(!visible);
    }
}

void AppRadar::handle_event(const RadarEvent& event, uint32_t now)
{
    auto& st = *_state;

    switch (event.type) {
        case RadarEventType::PhaseEntered:
            st.previous_phase = st.shown_phase;
            st.shown_phase    = event.phase;
            mclog::tagInfo(getAppInfo().name, "phase: {}", phaseName(event.phase));
            enter_phase_visuals(now);
            break;

        case RadarEventType::Ping:
            st.screen.ping(now);
            if (!SoundPlayer::instance().isBusy()) {
                SoundPlayer::instance().playSfx(Sfx::RadarPing);
            }
            break;

        case RadarEventType::Blip: {
            float distance = 0.0f;
            float range01  = 0.6f;
            if (st.mode == VisionMode::Face) {
                distance = faceDistanceMeters(event.size, kHalfFovYaw);
                range01  = distance / kMaxRangeM;
            }
            BlipKind kind = event.friendly    ? BlipKind::Friend
                            : event.unreachable ? BlipKind::Behind
                            : event.confirmed   ? BlipKind::Locked
                                                : BlipKind::Contact;
            st.screen.addBlip(event.bearing, range01, kind, now);
            st.screen.setInfo(distance > 0.0f ? fmt::format("{:.1f}m", distance) : std::string("动态"));
            break;
        }
    }
}

void AppRadar::enter_phase_visuals(uint32_t now)
{
    auto& st        = *_state;
    auto& stackchan = GetStackChan();
    auto& avatar    = stackchan.avatar();
    auto& sound     = SoundPlayer::instance();
    const auto& cfg = st.machine->config();

    avatar.clearDecorators();
    avatar.clearSpeech();
    st.gaze_x = 0;
    st.gaze_y = 0;
    avatar.leftEye().setPosition({0, 0});
    avatar.rightEye().setPosition({0, 0});

    switch (st.shown_phase) {
        case Phase::Scanning:
            if (st.previous_phase == Phase::Checking) {
                st.check_failed_ms    = now;  // spinning on without a lock: the face was not confirmed
                st.check_failed_shown = true;
            }
            st.lock_view.hide();
            set_avatar_visible(false);
            st.screen.setVisible(true);
            avatar.setEmotion(avatar::Emotion::Neutral);
            st.screen.setInfo("--");
            st.leds.set(fx::LedMode::Off);
            st.led_valid = false;
            update_status();
            break;

        case Phase::Locking: {
            st.screen.setVisible(false);
            set_avatar_visible(false);
            st.lock_view.show(now);
            if (st.mode == VisionMode::Face && st.face_frame.frame.pixels) {
                st.lock_view.setFrame(st.face_frame.frame);
                st.face_frame_fresh = false;
            }
            const Contact& c = st.last_contact;
            st.lock_view.setTarget(c.x, c.y, c.w, c.h,
                                   st.mode == VisionMode::Face ? faceDistanceMeters(c.w, kHalfFovYaw) : 0.0f);
            avatar.setEmotion(avatar::Emotion::Angry);
            sound.playSfxExclusive(Sfx::Alarm);
            st.leds.set(fx::LedMode::Pulse, kLedAngry, 600);
            st.leds.flash(kLedAlarm, cfg.alarmMs, now);
            st.hint_shown = false;
            if (st.mode == VisionMode::Face) {
                mclog::tagInfo(getAppInfo().name,
                               "target locked at yaw {} pitch {}: face at ({:.2f}, {:.2f}) size {:.2f} x {:.2f}, score "
                               "{:.2f}, {} face(s) in the frame",
                               st.machine->targetYaw(), st.machine->targetPitch(), c.x, c.y, c.w, c.h,
                               st.tracker.target().face.score, st.face_frame.faces.size());
            } else {
                mclog::tagInfo(getAppInfo().name, "target locked at yaw {} pitch {}: motion at ({:.2f}, {:.2f})",
                               st.machine->targetYaw(), st.machine->targetPitch(), c.x, c.y);
            }
            break;
        }

        case Phase::Angry:
            st.lock_view.hide();
            st.screen.setVisible(false);
            set_avatar_visible(true);
            avatar.setEmotion(avatar::Emotion::Angry);
            avatar.addDecorator(std::make_unique<avatar::AngryDecorator>(lv_screen_active(), 0, 400));
            st.leds.set(fx::LedMode::Pulse, kLedAngry, 900);
            break;

        case Phase::Cooldown:
            st.lock_view.hide();
            st.screen.setVisible(false);
            set_avatar_visible(true);
            if (st.previous_phase == Phase::Forgiven) {
                avatar.setEmotion(avatar::Emotion::Happy);
            } else {
                avatar.setEmotion(avatar::Emotion::Doubt);
                avatar.setSpeech("人呢？");
            }
            st.leds.set(fx::LedMode::Pulse, kLedCalm, 2200);
            break;

        case Phase::Forgiven:
            sound.stopAll();
            sound.playSfx(Sfx::Success);
            st.lock_view.hide();
            st.screen.setVisible(false);
            set_avatar_visible(true);
            avatar.setEmotion(avatar::Emotion::Happy);
            avatar.addDecorator(std::make_unique<avatar::HeartDecorator>(lv_screen_active(), 0, 500));
            avatar.setSpeech("好啦，不生气了～");
            st.leds.set(fx::LedMode::Pulse, kLedFriend, 1200);
            st.leds.flash(kLedFriend, 500, now);
            break;

        case Phase::Checking:
            // Braked for a face seen while spinning: the radar stays up while the head turns back to check it
            st.lock_view.hide();
            set_avatar_visible(false);
            st.screen.setVisible(true);
            st.leds.set(fx::LedMode::Pulse, kLedCheck, 500);
            st.led_valid = false;
            update_status();
            mclog::tagInfo(getAppInfo().name, "spin: face seen, checking at yaw {} pitch {}", st.machine->targetYaw(),
                           st.machine->targetPitch());
            break;
    }
}

void AppRadar::update_spin_inputs(uint32_t now, RadarInput& input)
{
    auto& st = *_state;
    if (!st.spinning) {
        return;
    }
    const bool looking = st.machine->lookingAround();
    if (looking != st.looking_logged) {
        st.looking_logged = looking;
        std::string stops;
        for (size_t i = 0; i < st.looked_count; i++) {
            stops += fmt::format(" {}", st.looked[i]);
        }
        mclog::tagInfo(getAppInfo().name, "spin: {} at yaw {}{}{}", looking ? "look-around turn" : "fast turns",
                       wrapYaw(st.spin_tracker.yaw()), stops.empty() ? "" : ", looked from", stops);
        st.looked_count = 0;
    }
    st.spin_tracker.setCommand(st.spin_velocity);
    st.spin_tracker.update(now, GetStackChan().motion().yawServo().readRawPosition());
    input.headValid    = true;
    input.headYaw      = st.spin_tracker.yaw();
    input.headVelocity = st.spin_tracker.velocity();
}

void AppRadar::update_spin_safety(uint32_t now)
{
    auto& st = *_state;
    // A 9 g servo spinning flat out for minutes heats up: slow down, pause when it gets hot
    if (!st.spinning || now - st.temp_checked < kTempCheckMs) {
        return;
    }
    st.temp_checked = now;
    const int temp  = GetStackChan().motion().yawServo().readTemperature();
    if (temp <= 0) {
        // No answer: a few in a row, and it is taken as hot rather than spun on blind
        if (++st.temp_fails >= kTempFailLimit && !st.servo_paused) {
            mclog::tagWarn(getAppInfo().name, "yaw servo temperature unreadable {} times: spin paused", st.temp_fails);
            st.servo_paused = true;
            st.machine->setSpinVelocity(0);
        }
        return;
    }
    st.temp_fails = 0;
    if (++st.temp_logs % 6 == 1) {  // every 30 s
        mclog::tagInfo(getAppInfo().name, "yaw servo at {} C", temp);
    }
    if (temp >= kServoHotC) {
        st.servo_paused = true;
    } else if (temp < kServoWarmC) {
        st.servo_paused = false;
    }
    const int velocity = st.servo_paused ? 0 : (temp >= kServoWarmC ? kSpinWarmVelocity : kSpinVelocity);
    if (velocity != st.machine->config().spinVelocity) {
        mclog::tagWarn(getAppInfo().name, "yaw servo at {} C: spin velocity {}", temp, velocity);
        st.machine->setSpinVelocity(velocity);
    }
}

void AppRadar::rotate_yaw(int velocity)
{
    auto& st = *_state;
    GetStackChan().motion().yawServo().rotate(velocity);
    st.spin_velocity = velocity;
    st.spin_tracker.setCommand(velocity);  // where the sensor cannot tell, the tracker goes by the command
}

void AppRadar::watch_stall(uint32_t now)
{
    auto& st = *_state;
    st.stall_watch.reset(now, st.spin_tracker.takenYaw());
}

bool AppRadar::spin_stalled(uint32_t now)
{
    auto& st          = *_state;
    const int yaw_way = (st.spin_velocity > 0 ? 1 : -1) * kSpinVelocityPerYaw;
    return st.stall_watch.update(now, st.spin_velocity, yaw_way, st.spin_tracker);
}

void AppRadar::handle_stall(uint32_t now)
{
    auto& st = *_state;
    if (st.stalls == 0 || now - st.stalls_from_ms > kStallWindowMs) {
        st.stalls         = 0;
        st.stalls_from_ms = now;
    }
    st.stalls++;
    rotate_yaw(0);
    if (st.stalls >= kMaxStalls) {
        mclog::tagWarn(getAppInfo().name, "spin: the head is stuck ({} stalls in a minute), sweeping instead",
                       st.stalls);
        st.spin_stuck = true;
        st.machine->setSpinScan(false, now);  // position mode from the next tick
        return;
    }
    mclog::tagWarn(getAppInfo().name, "spin: the head does not turn, resting {} ms", kStallBackoffMs);
    st.backing_off   = true;
    st.backoff_until = now + kStallBackoffMs;
}

bool AppRadar::leave_spin(uint32_t now, int targetYaw)
{
    auto& st     = *_state;
    auto& motion = GetStackChan().motion();
    if (!st.escaping) {
        st.escaping        = true;
        st.backing_off     = false;
        st.escape_from_ms  = now;
        const int yaw_way  = wrapYaw(targetYaw - wrapYaw(st.spin_tracker.yaw())) >= 0 ? 1 : -1;
        st.escape_velocity = yaw_way * kSpinVelocityPerYaw * kDeadBandEscapeVelocity;
        if (st.spin_velocity != 0) {
            rotate_yaw(0);  // coast down: position mode at speed would brake hard against the gears
        }
    }
    const uint32_t spent = now - st.escape_from_ms;
    if (std::fabs(st.spin_tracker.velocity()) > kStopRate && spent < kCoastMs) {
        return false;
    }
    if (!st.spin_tracker.clearOfDeadBand()) {
        if (spent < kDeadBandEscapeMs) {
            if (st.escape_velocity != st.spin_velocity) {
                rotate_yaw(st.escape_velocity);
            }
            st.spin_tracker.update(now, motion.yawServo().readRawPosition());
            if (!st.spin_tracker.clearOfDeadBand()) {
                return false;
            }
        } else if (st.machine->phase() == Phase::Checking) {
            // Could not get off it (held, or the sensor failed): give the face up, stay in wheel mode at rest
            mclog::tagWarn(getAppInfo().name, "spin: the head did not leave the yaw sensor's dead band, check given up");
            rotate_yaw(0);
            st.escaping = false;
            st.machine->cancelCheck(now);
            return false;
        }
        // Otherwise position mode all the same: with no reading to hold, the servo turns its torque off first
    }
    // Synchronous: PWM 0, back to position mode, holding where it is (the next move starts from there)
    motion.yawServo().stopRotation();
    motion.setAutoTorqueReleaseEnabled(true);
    st.spin_velocity = 0;
    st.spin_tracker.setCommand(0);
    st.spinning = false;
    st.escaping = false;
    return true;
}

void AppRadar::note_look_stop()
{
    // A look-around stop ends (logged per turn)
    auto& st = *_state;
    if (st.servo_paused || st.looked_count >= st.looked.size()) {
        return;
    }
    st.looked[st.looked_count++] = wrapYaw(st.spin_tracker.yaw());
}

void AppRadar::turn_to_brake_point(int brakeAtYaw)
{
    // Blocking for less than a tick: read the sensor until the head is there (a reading takes about 0.5 ms). Above
    // the face detector meanwhile: it shares this core at this loop's priority, and every wait for the servo's answer
    // would hand it a whole 10 ms time slice (its frames are blurred while turning anyway).
    auto& st                  = *_state;
    auto& yaw                 = GetStackChan().motion().yawServo();
    const int yaw_way         = (st.spin_velocity > 0 ? 1 : -1) * kSpinVelocityPerYaw;
    const uint32_t from       = GetHAL().millis();
    const UBaseType_t priority = uxTaskPriorityGet(nullptr);
    vTaskPrioritySet(nullptr, priority + 1);
    while ((brakeAtYaw - st.spin_tracker.yaw()) * yaw_way > 0 && GetHAL().millis() - from < kBrakeWaitMs) {
        st.spin_tracker.update(GetHAL().millis(), yaw.readRawPosition());
    }
    vTaskPrioritySet(nullptr, priority);
}

void AppRadar::stop_spinning_now()
{
    auto& st = *_state;
    auto& yaw = GetStackChan().motion().yawServo();
    if (st.spinning) {
        // Off the dead band first (blocking, at most kCloseEscapeMs), the shorter way round to the front
        const uint32_t from = GetHAL().millis();
        while (GetHAL().millis() - from < kCloseEscapeMs) {
            const uint32_t now = GetHAL().millis();
            st.spin_tracker.setCommand(st.spin_velocity);
            st.spin_tracker.update(now, yaw.readRawPosition());
            if (leave_spin(now, 0)) {
                return;
            }
            GetHAL().delay(10);
        }
    }
    yaw.stopRotation();  // a no-op in position mode
}

void AppRadar::apply_head(uint32_t now)
{
    auto& st         = *_state;
    auto& motion     = GetStackChan().motion();
    const auto& head = st.machine->head();

    if (head.rotate) {
        if (st.streaming) {
            motion.setAutoAngleSyncEnabled(true);
            st.streaming      = false;
            st.streamed_yaw   = INT_MIN;
            st.streamed_pitch = INT_MIN;
        }
        if (head.serial != st.applied_serial) {
            st.applied_serial = head.serial;
            motion.pitchServo().moveWithSpeed(head.pitch, kPitchSpeed);
        }
        if (!st.spinning) {
            // The yaw spring must be at rest first: its next position write would end the wheel mode (isMoving()
            // asks the servo over UART: 20 Hz)
            if (!st.spin_waiting) {
                st.spin_waiting = true;
                st.spin_wait_ms = now;
                st.spin_poll_ms = 0;
            }
            if (now - st.spin_poll_ms < 50) {
                return;
            }
            st.spin_poll_ms = now;
            if (motion.yawServo().isMoving() && now - st.spin_wait_ms < kSpinStartWaitMs) {
                return;
            }
            // The tracker counts from where the head is, with the servo's own zero (NVS); the angle from the reading
            // (getCurrentYawAngle() is clamped at the limits, the head may be further round)
            const int raw  = motion.yawServo().readRawPosition();
            const int zero = motion.yawServo().getZeroRawPosition();
            auto tracker   = st.spin_tracker.config();
            if (zero >= tracker.rawMin && zero <= tracker.rawMax) {
                tracker.zeroRaw = zero;
                st.spin_tracker.setConfig(tracker);
            }
            const int yaw = raw >= tracker.rawMin && raw <= tracker.rawMax
                                ? static_cast<int>(std::lround((raw - tracker.zeroRaw) * tracker.yawPerRaw))
                                : motion.getCurrentYawAngle();
            // Look-around stops where the sensor reads, off its dead band
            st.machine->setLookRange(
                static_cast<int>(std::lround((tracker.rawMin - tracker.zeroRaw) * tracker.yawPerRaw)) + kLookEdgeMargin,
                static_cast<int>(std::lround((tracker.rawMax - tracker.zeroRaw) * tracker.yawPerRaw)) - kLookEdgeMargin);
            st.spin_tracker.reset(now, yaw);
            motion.setAutoTorqueReleaseEnabled(false);  // it would stop the rotation
            rotate_yaw(head.velocity);
            watch_stall(now);
            st.spinning     = true;
            st.spin_waiting = false;
            st.backing_off  = false;
            mclog::tagInfo(getAppInfo().name, "spinning at {} from yaw {} (zero {})", head.velocity, yaw,
                           tracker.zeroRaw);
            return;
        }
        st.escaping = false;  // spinning on (a check given up)
        if (st.backing_off) {
            if (static_cast<int32_t>(now - st.backoff_until) < 0) {
                return;
            }
            st.backing_off = false;
            mclog::tagInfo(getAppInfo().name, "spin: trying again");
            rotate_yaw(head.velocity);  // also brings it back to wheel mode if a late position write ended it
            watch_stall(now);
            return;
        }
        if (head.brakeArmed && head.brakeSerial != st.brake_serial) {
            // A look-around stop: on (from rest too) until the sensor says the head is at the brake point, then rest
            st.brake_serial = head.brakeSerial;
            if (st.spin_velocity == 0) {
                note_look_stop();
            }
            if (st.spin_velocity != head.brakeVelocity) {
                rotate_yaw(head.brakeVelocity);
            }
            turn_to_brake_point(head.brakeAtYaw);
            rotate_yaw(0);
            return;
        }
        if (head.velocity != st.spin_velocity) {
            if (st.spin_velocity == 0) {
                note_look_stop();
            }
            rotate_yaw(head.velocity);
        } else if (spin_stalled(now)) {
            handle_stall(now);
        }
        return;
    }
    st.spin_waiting = false;
    if (st.spinning && !leave_spin(now, head.yaw)) {
        return;
    }

    if (head.stream) {
        if (!st.streaming) {
            // Retargeting every tick: keep the spring's momentum instead of re-reading the servo each time
            motion.setAutoAngleSyncEnabled(false);
            st.streaming = true;
        }
        if (head.yaw != st.streamed_yaw) {
            st.streamed_yaw = head.yaw;
            motion.yawServo().moveWithSpeed(head.yaw, kStreamSpeed);
        }
        if (head.pitch != st.streamed_pitch) {
            st.streamed_pitch = head.pitch;
            motion.pitchServo().moveWithSpeed(head.pitch, kPitchSpeed);
        }
        return;
    }

    if (st.streaming) {
        motion.setAutoAngleSyncEnabled(true);
        st.streaming      = false;
        st.streamed_yaw   = INT_MIN;
        st.streamed_pitch = INT_MIN;
    }
    if (head.serial != st.applied_serial) {
        st.applied_serial = head.serial;
        motion.moveWithSpeed(head.yaw, head.pitch, head.speed);
    }
}

void AppRadar::update_status()
{
    auto& st = *_state;
    const uint32_t now = GetHAL().millis();
    if (st.mode == VisionMode::Blind) {
        st.screen.setStatus("摄像头不可用", kTextRed);
    } else if (st.mode == VisionMode::Face && !st.detector_ready) {
        st.screen.setStatus("预热中…", kTextYellow);
    } else if (st.mean_luma >= 0.0f && st.mean_luma < kDarkLuma) {
        st.screen.setStatus("光线太暗", kTextOrange);
    } else if (st.machine->phase() == Phase::Checking) {
        st.screen.setStatus("发现目标？", kTextYellow);
    } else if (st.check_failed_shown && now - st.check_failed_ms < kCheckFailedShowMs) {
        st.screen.setStatus("没看清", kTextYellow);
    } else if (st.spin_stuck) {
        // Alternating with how to get it going again (the radar screen has no speech bubble)
        st.screen.setStatus((now / 2000) % 2 == 0 ? "头被卡住了？" : "摸摸头恢复转动", kTextOrange);
    } else if (st.servo_paused) {
        st.screen.setStatus("舵机降温中", kTextOrange);
    } else if (st.remote_session && now - st.remote_since_ms < kRemoteHintMs && (now / 2000) % 2 == 1) {
        st.screen.setStatus("按 M5 键回桌面", kTextCyan);
    } else if (st.machine->inFriendGrace(now)) {
        st.screen.setStatus("友好模式", kTextCyan);
    } else if (st.mode == VisionMode::Motion) {
        st.screen.setStatus("动态侦测", kTextGreen);
    } else if (st.machine->config().spinScan) {
        st.screen.setStatus(st.machine->lookingAround() ? "环顾搜索" : "快速扫描", kTextGreen);
    } else {
        st.screen.setStatus("扫描中", kTextGreen);
    }
}

void AppRadar::update_screen(uint32_t now)
{
    auto& st     = *_state;
    auto& avatar = GetStackChan().avatar();
    const Phase phase = st.machine->phase();

    switch (phase) {
        case Phase::Scanning: {
            // A slow detector slows the sweep down, so a face still gets enough looks while it is in view
            const bool spin = st.machine->config().spinScan;
            if (!spin && st.mode == VisionMode::Face && now - st.speed_checked >= 1000) {
                st.speed_checked = now;
                const int speed  = st.detector_ready
                                       ? sweepSpeedForRate(st.detect_fps, kHalfFovYaw * 2, kFramesPerView,
                                                           kSweepMinSpeed, kSweepMaxSpeed)
                                       : kSweepMaxSpeed;
                if (speed != st.sweep_speed) {
                    st.sweep_speed = speed;
                    st.machine->setSweepSpeed(speed);
                    mclog::tagInfo(getAppInfo().name, "sweep {} deg/s for {:.1f} detector fps", speed / 10,
                                   st.detect_fps);
                }
            }
            st.screen.setSweep(st.machine->sweepYaw(), st.machine->sweepVelocity(),
                               spin ? kSpinYawRate : static_cast<float>(st.sweep_speed));
            if (now - st.bearing_shown_ms >= 200) {
                st.bearing_shown_ms = now;
                st.screen.setBearing(static_cast<int>(std::lround(st.machine->sweepYaw())));
                update_status();
            }
            st.screen.update(now);
            break;
        }

        case Phase::Locking:
            if (st.mode == VisionMode::Face) {
                if (st.face_frame_fresh) {
                    st.face_frame_fresh = false;
                    st.lock_view.setFrame(st.face_frame.frame);
                    const auto& t = st.tracker.target();
                    if (t.present) {
                        st.lock_view.setTarget(t.face.cx, t.face.cy, t.face.w, t.face.h,
                                               faceDistanceMeters(t.face.w, kHalfFovYaw));
                    }
                }
            } else if (st.mode == VisionMode::Motion) {
                // No detector frames in the fallback: show the camera directly
                RgbFrame frame;
                FrameGrabber::instance().requestRgb();
                if (FrameGrabber::instance().latestRgb(st.rgb_seq, frame)) {
                    st.rgb_seq = frame.seq;
                    st.lock_view.setFrame(frame);
                }
            }
            st.lock_view.update(now);
            break;

        case Phase::Angry: {
            // Glare: the eyes follow what is left of the aim error (the head does the big part)
            const auto& t = st.tracker.target();
            int gx        = 0;
            int gy        = 0;
            if (st.mode == VisionMode::Face && t.present) {
                gx = std::clamp(st.machine->yawOffsetFor(t.face.cx) * kGazeRange / kHalfFovYaw, -kGazeRange,
                                kGazeRange);
                gy = std::clamp(-st.machine->pitchOffsetFor(t.face.cy) * kGazeRange / kHalfFovPitch, -kGazeRange,
                                kGazeRange);
            }
            if (std::abs(gx - st.gaze_x) >= 3 || std::abs(gy - st.gaze_y) >= 3) {
                st.gaze_x = gx;
                st.gaze_y = gy;
                avatar.leftEye().setPosition({gx, gy});
                avatar.rightEye().setPosition({gx, gy});
            }
            if (!st.hint_shown && now - st.machine->lockStartMs() >= kHintDelayMs) {
                st.hint_shown = true;
                avatar.setSpeech("摸摸头，我就不生气");
            }
            break;
        }

        case Phase::Checking:
            // The radar keeps showing where the head points while it turns back to the face
            st.screen.setSweep(st.machine->sweepYaw(), 0.0f, kSpinYawRate);
            st.screen.update(now);
            break;

        case Phase::Cooldown:
        case Phase::Forgiven:
            break;
    }
}

void AppRadar::update_scanner_leds(uint32_t now)
{
    auto& st = *_state;
    if (now - st.led_written_ms < fx::LedAnimator::kFrameMs) {
        return;
    }
    // A soft green dot running along both LED rows with the head: back and forth with the sweep ("KITT" scanner),
    // round and round (wrapping from the last LED to the first) with the spin
    const bool spin = st.machine->config().spinScan;
    float head      = 0.0f;
    if (spin) {
        head = (st.machine->sweepYaw() + 1800.0f) / 3600.0f * 6.0f;
    } else {
        const float pos = (st.machine->sweepYaw() + kYawLimit) / (2.0f * kYawLimit);
        head            = std::clamp(pos, 0.0f, 1.0f) * 5.0f;
    }
    std::array<uint8_t, 6> levels{};
    for (int i = 0; i < 6; i++) {
        float distance = std::fabs(static_cast<float>(i) - head);
        if (spin) {
            distance = std::min(distance, 6.0f - distance);
        }
        float level = std::clamp(1.0f - distance / 1.6f, 0.0f, 1.0f);
        level       = level * level;
        levels[i]   = static_cast<uint8_t>(std::lround(level * 15.0f)) * 17;  // 16 steps: fewer I2C refreshes
    }
    if (st.led_valid && levels == st.led_levels) {
        return;
    }
    st.led_levels     = levels;
    st.led_valid      = true;
    st.led_written_ms = now;
    for (int i = 0; i < 6; i++) {
        const uint8_t g = static_cast<uint8_t>(levels[i] * kScanLedMax / 255);
        const uint8_t b = g / 4;
        GetHAL().setRgbColor(i, 0, g, b);
        GetHAL().setRgbColor(6 + i, 0, g, b);
    }
    GetHAL().refreshRgb();
}

void AppRadar::update_leds(uint32_t now)
{
    auto& st = *_state;
    st.leds.update(now);
    if (st.machine->phase() == Phase::Scanning) {
        update_scanner_leds(now);
    }
}

void AppRadar::onClose()
{
    // First of all: never leave the yaw servo in wheel mode (the spin scan uses it)
    stop_spinning_now();

    mclog::tagInfo(getAppInfo().name, "on close");

    auto& st = *_state;
    st.head_touch.detach();
    vision::FaceDetector::instance().stop();
    FrameGrabber::instance().stop();
    SoundPlayer::instance().shutdown();

    {
        LvglLockGuard lock;

        auto& motion = GetStackChan().motion();
        motion.setAutoAngleSyncEnabled(true);
        motion.setAutoTorqueReleaseEnabled(true);

        GetStackChan().clearModifiers();
        st.lock_view.destroy();
        st.screen.destroy();
        GetStackChan().resetAvatar();

        view::destroy_home_indicator();
        view::destroy_status_bar();
    }
    _state.reset();

    GetHAL().showRgbColor(0, 0, 0);
    GetHAL().requestWarmReboot(stackpet::kAppIndexRadar);
}
