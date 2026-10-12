/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * Pure C++ logic of the RADAR app (no ESP-IDF / LVGL dependencies, host tested by tests/stackpet_radar_*).
 *
 * Units: head angles in 0.1 degree (the stackchan::motion convention, +yaw = towards the robot's left), times in
 * milliseconds from GetHAL().millis(), image coordinates normalized to -1..1 (+x = right, +y = down, as captured).
 */
namespace stackpet::radar {

struct SweepConfig {
    int limit        = 1250;  // sweep between -limit .. +limit
    int speed        = 300;   // cruise speed, 0.1 degree per second
    int accel        = 600;   // 0.1 degree per second^2
    int dwellEvery   = 0;     // 0 = continuous sweep
    uint32_t dwellMs = 1000;
};

/**
 * Back-and-forth head sweep: cruise at a constant speed, brake with a constant deceleration so the head reaches
 * zero speed exactly at the limit, then accelerate the other way (no jerk at the ends). Optional dwell for the
 * motion fallback: stop after every `dwellEvery` of travel and hold still for `dwellMs`.
 */
class SweepPlanner {
public:
    using Config = SweepConfig;

    explicit SweepPlanner(const Config& config = Config());

    void setConfig(const Config& config);  // keeps the current position and velocity
    const Config& config() const;
    void setSpeed(int speed);

    // Restart from `position` at rest, heading towards `direction` (+1 / -1).
    void reset(float position, int direction);
    // Advance to nowMs. The first call after reset() only records the time.
    void update(uint32_t nowMs);

    float position() const;
    float velocity() const;  // signed, 0.1 degree per second
    int direction() const;   // where the head is heading (or will head after the dwell)
    bool dwelling() const;   // at rest in a dwell
    uint32_t dwellStartMs() const;

private:
    enum class State : uint8_t { Moving, Braking, Dwelling };

    Config _config;
    State _state         = State::Moving;
    float _position      = 0.0f;
    float _velocity      = 0.0f;
    int _direction       = 1;
    float _traveled      = 0.0f;
    uint32_t _last_ms    = 0;
    bool _has_time       = false;
    uint32_t _dwell_from = 0;
};

enum class Phase : uint8_t {
    Scanning,  // radar screen, head sweeping
    Locking,   // camera view with the lock box, alarm (first lockViewMs of the alarm)
    Angry,     // angry face glaring at the target while it stays
    Cooldown,  // target gone: calm down, then scan again
    Forgiven,  // head petted: happy, no alarm for friendGraceMs
    Checking,  // spin scan: braked for a face seen while spinning, turned back to it, confirming on still frames
};

const char* phaseName(Phase phase);

struct RadarConfig {
    SweepConfig sweep;
    int scanPitch     = 120;
    int aimYawLimit   = 1280;  // servo range
    int pitchMin      = 0;
    int pitchMax      = 650;
    int halfFovYaw    = 330;   // yaw offset of a target at the image edge
    int halfFovPitch  = 250;
    int imageXToYaw   = -1;    // yaw direction of a target right of the image centre
    int imageYToPitch = -1;    // pitch direction of a target below the centre
    int servoLagMs    = 120;   // the real head trails the commanded sweep by about this much

    // Spin scan (wheel mode, user: "SPIN 速度转两圈再停着看一圈", always the same way round): spinTurns fast turns at spinVelocity
    // (Servo::rotate() units, -1000..1000; 900 = the SPIN game, about 400 degree/s), then one look-around turn
    // stopping lookStops times (frames taken while spinning are too blurred to find faces in, still ones are fine),
    // then fast turns again. A face seen in any frame makes the head turn to it and confirm it on still frames
    // first (Phase::Checking).
    bool spinScan           = false;  // false: back-and-forth sweep (SweepPlanner; the motion fallback uses it)
    int spinVelocity        = 900;
    int spinVelocityPerYaw  = -1;     // sign of the rotate() velocity that turns towards +yaw
    float spinTurns         = 2.0f;   // fast turns (the spin's way round) before each look-around turn
    // The look-around turn stops at lookStops fixed bearings, evenly from lookFromYaw to lookToYaw (wrapped): where the
    // yaw sensor reads, off its dead band behind the robot (the app sets them from the servo's zero), so the head never
    // rests in the dead band; the bit behind is seen from the two outer stops (the camera sees 52 degree)
    int lookStops           = 8;
    int lookFromYaw         = -1337;
    int lookToYaw           = 1587;
    int lookLeadYaw         = 400;    // the first stop is at least this far ahead of the head (it has to brake)
    int lookVelocity        = 700;    // rotate() speed between the stops
    // Stop rotating this long (at the head's speed) before a stop: with PWM 0 the head freewheels on, about 17 degree
    // from 700 on the CoreS3 (2026-10-12, 6 stops: 54 ms worth)
    uint32_t lookCoastMs    = 55;
    float spinRatePerUnit   = 4.6f;   // 0.1 degree/s per rotate() unit: how fast the head should be going
    uint32_t lookSettleMs   = 150;    // frames taken this long after the stop are still
    int lookFrames          = 2;      // still frames to look at per stop ...
    uint32_t lookMaxMs      = 700;    // ... or this long at most
    uint32_t checkMs        = 1500;   // after the turn to a face: time to confirm it on still frames
    int frameLatencyMs      = 40;     // spinning: a frame was exposed about this long before its timestamp
    // Faces the head cannot face (further than aimYawLimit + reachFov x halfFovYaw, i.e. behind the robot) only
    // show up as blips: checking them would time out every time and the spin would keep coming back to them
    float reachFov          = 0.8f;
    // A face that could not be confirmed is not checked again from about the same bearing for a while
    uint32_t checkMuteMs    = 20000;
    int checkMuteYaw        = 250;

    int confirmHits         = 2;      // consecutive vision results with the target before locking on
    uint32_t pingPeriodMs   = 1800;
    uint32_t lockViewMs     = 3000;  // user: "聚焦到人上,闪着红光并发出警报声3秒,然后…生气" -> camera view for the whole alarm
    uint32_t alarmMs        = 3000;
    uint32_t glareMs        = 3000;   // minimum angry stare after the lock view
    uint32_t lostMs         = 1500;   // target unseen this long = it left
    uint32_t cooldownMs     = 4000;
    uint32_t forgivenMs     = 2500;
    // After a head pet nobody is treated as an intruder until nobody has been seen for this long: every sighting
    // during the grace renews it, so the person who petted the head can stay (and a face on a poster keeps it going)
    uint32_t friendGraceMs  = 30000;
    uint32_t settleMs       = 450;    // frames captured sooner after a head move are not used to correct the aim

    int aimSpeed      = 450;  // Motion::moveWithSpeed speed of the lock-on turn
    int trackSpeed    = 300;  // ... of the small corrections while glaring
    int returnSpeed   = 250;
    int trackDeadband = 25;   // aim errors below this are ignored
    float trackGain   = 0.7f;
};

// What the vision side (face tracker, or the motion fallback) reports for this tick.
struct Contact {
    bool fresh       = false;  // a new vision result arrived this tick (it may contain no target)
    bool present     = false;  // that result has a target
    int hits         = 0;      // consecutive results with the target
    float x          = 0.0f;   // target centre in the image
    float y          = 0.0f;
    float w          = 0.0f;   // target size, fraction of the image width / height
    float h          = 0.0f;
    uint32_t captureMs = 0;    // capture time of the analyzed frame
};

struct RadarInput {
    uint32_t nowMs = 0;
    Contact contact;
    bool petted = false;
    // Spin scan: the measured head yaw (SpinTracker: unwrapped, 0.1 degree) and its velocity, every tick while the
    // yaw servo is in wheel mode (spinning, or leaving the sensor's dead band before a position move)
    bool headValid     = false;
    int headYaw        = 0;
    float headVelocity = 0.0f;  // 0.1 degree per second
};

enum class RadarEventType : uint8_t {
    PhaseEntered,
    Ping,  // time for a sonar ping (scanning only)
    Blip,  // a target was seen (scanning only), for the radar screen
};

struct RadarEvent {
    RadarEventType type = RadarEventType::PhaseEntered;
    Phase phase         = Phase::Scanning;  // PhaseEntered
    int bearing         = 0;                // Blip: head yaw that faces the target
    float size          = 0.0f;             // Blip: target width fraction (distance estimate)
    bool confirmed      = false;            // Blip: enough hits to lock on
    bool friendly       = false;            // Blip: seen during the friend grace, no alarm
    bool unreachable    = false;            // Blip: behind the robot, out of the head's reach (spin scan)
};

/**
 * Head command for this tick. While `rotate` is set the yaw servo spins (Servo::rotate(velocity), wheel mode) and only
 * the pitch follows `pitch` (on `serial` changes). While `stream` is set the app follows yaw/pitch every tick (the
 * sweep, auto angle sync off); otherwise it issues one Motion::moveWithSpeed() whenever `serial` changes.
 */
struct HeadCommand {
    bool rotate     = false;
    int velocity    = 0;
    // Spin scan, braking for a look-around stop (velocity 0): the head gets to brakeAtYaw (unwrapped, 0.1 degree)
    // before the next tick would come, so the app turns it at brakeVelocity (from rest too) and watches the sensor
    // until then, once per brakeSerial
    bool brakeArmed      = false;
    int brakeAtYaw       = 0;
    int brakeVelocity    = 0;
    uint32_t brakeSerial = 0;
    bool stream     = false;
    int yaw         = 0;
    int pitch       = 0;
    int speed       = 500;
    uint32_t serial = 0;
};

class RadarMachine {
public:
    explicit RadarMachine(const RadarConfig& config);

    // Start scanning from the current head position.
    void start(uint32_t nowMs, int yaw, int pitch);
    void update(const RadarInput& input);
    std::vector<RadarEvent> takeEvents();

    Phase phase() const;
    uint32_t phaseStartMs() const;
    const HeadCommand& head() const;
    const RadarConfig& config() const;

    float sweepYaw() const;       // the radar line: commanded sweep position, or the measured head yaw when spinning
    float sweepVelocity() const;  // signed, 0.1 degree per second
    int sweepDirection() const;
    bool dwellSettled(uint32_t nowMs) const;  // motion fallback: head at rest long enough to compare frames
    bool alarmActive(uint32_t nowMs) const;    // within alarmMs of the last lock-on (not forgiven)
    bool inFriendGrace(uint32_t nowMs) const;
    uint32_t lockStartMs() const;

    void setSweepSpeed(int speed);
    void setDwellScan(int dwellEvery, uint32_t dwellMs);  // 0 = continuous sweep
    // Spin scan on/off at run time (the motion fallback needs the back-and-forth sweep with its dwells): a scan in
    // progress restarts in the new mode from where the head is
    void setSpinScan(bool enabled, uint32_t nowMs);
    void setLookRange(int fromYaw, int toYaw);  // spin scan: the look-around stops' bearings (0.1 degree)
    int lookStopYaw(int index) const;           // ... the index-th of them, wrapped
    // Checking a face, but the head could not get there (stuck in the yaw sensor's dead band): give up, spin on
    void cancelCheck(uint32_t nowMs);
    void setSpinVelocity(int velocity);      // spin scan speed (e.g. lowered while the yaw servo runs hot)
    int spinDirection() const;               // spin scan: +1 = towards +yaw
    bool lookingAround() const;              // spin scan: in the look-around turn (stopping, not fast turns)
    bool reachable(int bearing) const;       // the head can turn far enough to see a face there and confirm it

    // Geometry
    int yawOffsetFor(float imageX) const;
    int pitchOffsetFor(float imageY) const;
    // Where the head pointed when a frame captured at `ms` was taken (sweep history, else the last move target)
    int yawAt(uint32_t ms) const;
    int pitchAt(uint32_t ms) const;
    int bearingOf(const Contact& contact) const;  // head yaw that faces the contact (wrapped to -1800..1799)
    int targetYaw() const;
    int targetPitch() const;

private:
    struct HistoryEntry {
        uint32_t ms   = 0;
        int yaw       = 0;
        int pitch     = 0;
        bool step     = false;  // the head was told to jump here (a move), no interpolation towards it
        bool measured = false;  // a spin scan reading (unwrapped yaw) rather than a commanded position
    };

    void enter(Phase phase, uint32_t nowMs);
    void aim_at(const Contact& contact, uint32_t nowMs, int speed);
    void move_to(int yaw, int pitch, int speed, uint32_t nowMs);
    void track(const Contact& contact, uint32_t nowMs);
    void record(uint32_t nowMs, int yaw, int pitch, bool step, bool measured = false);
    void set_spin_command(int speed);
    void update_spin_scan(const RadarInput& input);
    void start_fast_turns();
    void start_look_around(uint32_t nowMs);
    void next_stop(uint32_t nowMs);
    bool arm_brake(uint32_t nowMs, bool headValid);  // moving to a stop: brake (with the app's help) if it is close
    int look_stop_after(int yaw) const;  // the first look-around stop beyond yaw (unwrapped) the spin's way round
    bool muted(int bearing, uint32_t nowMs) const;
    void mute(int bearing, uint32_t nowMs);
    bool confirmed(const Contact& contact) const;
    void emit_blip(const Contact& contact, uint32_t nowMs);

    RadarConfig _config;
    SweepPlanner _planner;
    Phase _phase             = Phase::Scanning;
    uint32_t _phase_start    = 0;
    HeadCommand _head;
    std::vector<RadarEvent> _events;
    std::vector<HistoryEntry> _history;  // ring buffer, most recent at _history_head - 1
    size_t _history_head     = 0;
    size_t _history_count    = 0;
    uint32_t _last_record_ms = 0;
    uint32_t _next_ping_ms   = 0;
    uint32_t _last_seen_ms   = 0;
    uint32_t _last_move_ms   = 0;
    uint32_t _lock_start_ms  = 0;
    bool _locked_once        = false;
    bool _alarm_cancelled    = false;
    uint32_t _friend_until   = 0;
    bool _friend_active      = false;
    int _target_yaw          = 0;
    int _target_pitch        = 0;
    // Spin scan
    enum class SpinStage : uint8_t { Fast, Step, Stop };  // fast turns; look-around: moving to a stop, stopped
    SpinStage _spin_stage    = SpinStage::Fast;
    int _spin_direction      = 1;      // one way for good (the USB cable is on the base, the head turns freely)
    float _fast_travel       = 0.0f;   // rotation (either way) of the current fast turns, 0.1 degree
    int _last_head_yaw       = 0;
    bool _last_head_set      = false;
    int _stop_index          = 0;      // look-around: stops done
    int _stop_yaw            = 0;      // unwrapped yaw of the next stop
    uint32_t _tick_ms        = 0;      // time between the last two updates
    uint32_t _last_update_ms = 0;
    bool _updated            = false;
    uint32_t _stop_ms        = 0;      // when the head stopped (or started moving to the stop)
    int _still_frames        = 0;
    bool _head_valid         = false;  // a measured head yaw arrived (this scan)
    int _head_yaw            = 0;
    float _head_velocity     = 0.0f;
    int _check_yaw           = 0;      // where the face seen while spinning should be
    int _check_pitch         = 0;
    int _check_hits          = 0;      // consecutive still frames with the face
    uint32_t _check_deadline = 0;
    int _check_bearing       = 0;      // the face's bearing as seen (unclamped), muted if it cannot be confirmed
    struct MutedBearing {
        int bearing    = 0;
        uint32_t until = 0;
        bool active    = false;
    };
    MutedBearing _muted[4];
};

// Sweep speed (0.1 degree per second) that keeps a target in view for at least `framesPerView` detector runs
// while the head sweeps past it: a slow detector slows the sweep down instead of missing people.
int sweepSpeedForRate(float resultsPerSecond, int fullFovYaw, float framesPerView, int minSpeed, int maxSpeed);

// Rough distance (metres) of a face from its box width as a fraction of the image width.
float faceDistanceMeters(float widthFraction, int halfFovYaw);

// A yaw (0.1 degree) wrapped to -1800 .. 1799.
int wrapYaw(int yaw);

struct SpinTrackerConfig {
    int rawMin          = 0;       // the yaw servo's position sensor range: readings outside it are its dead band
    int rawMax          = 1000;
    int zeroRaw         = 460;     // reading at yaw 0 (the servo's calibrated zero)
    float yawPerRaw     = 3.125f;  // 0.1 degree per sensor step, as the servo driver maps them
    float velocityAlpha = 0.5f;    // weight of a new velocity sample against the command's
    // Without readings to go by (dead band, glitches, no answer) the head follows the rotate() command
    // (setCommand()): rotate(+v) turns towards -yaw at about 0.46 degree/s per unit from 300 to 900 (a turn took
    // 1.08 s at 700 on the CoreS3), reached with this time constant
    float yawRatePerUnit = -4.6f;
    float commandTauMs   = 30.0f;
    float slowTauMs      = 55.0f;  // slowing down (PWM 0 freewheels: 17 degree from 700, 54 ms worth, on the CoreS3)
    // The dead band is about 42 degree of the head's turn but 47.5 in the readings' terms (a turn takes 12 % longer
    // than 1000 steps at the speed in range, cal_session.log): in it the estimate goes this much faster
    float bandRateScale = 1.15f;
    // A reading further than glitchMargin + rateSlack x (the predicted turn) from where the command has the head is
    // not trusted: a glitch (single wrong values such as 221 or 801 come right next to the dead band) or the estimate
    // went off (the head stalled in the dead band, or was held). confirmReadings of them in a row that agree with each
    // other (no faster than maxRate apart) and are reachable from the last reading taken are taken when they make
    // sense: moving as the command says (within confirmRateSlack, over confirmMoveMs at least), close to the estimate
    // while the head rests (restResyncYaw, over confirmRestMs; never far: some glitch values repeat, far off, while it
    // rests at the edge of the dead band), or anyway after confirmHoldMs (a held head). Readings can come 1 ms apart
    // (the app watching for a brake point), and a glitch can repeat that fast.
    float glitchMargin  = 50.0f;    // 0.1 degree
    float rateSlack     = 0.3f;
    float maxRate       = 5000.0f;  // 0.1 degree per second, also the most the velocity estimate says
    int confirmReadings      = 3;
    float confirmRateSlack   = 0.5f;
    uint32_t confirmMoveMs   = 15;
    uint32_t confirmRestMs   = 30;
    uint32_t confirmHoldMs   = 250;
    float restResyncYaw      = 300.0f;  // 0.1 degree
    int edgeMargin      = 20;   // clearOfDeadBand(): sensor steps inside the range (the glitches come at its edges)
    uint32_t velocitySpanMs = 15;  // velocity samples span at least this long (readings can come 0 .. 1 ms apart)
};

/**
 * Head yaw while the yaw servo spins in wheel mode, from its position sensor readings. Unwrapped: it keeps counting
 * over full turns, so a turn is 3600 more. Through the sensor's dead band (about 48 degree, it reads above rawMax
 * there), over missed readings and over glitches it follows the rotate() command (setCommand()), so a head that
 * stops in the dead band stops in the estimate too.
 */
class SpinTracker {
public:
    using Config = SpinTrackerConfig;

    explicit SpinTracker(const Config& config = Config());
    void setConfig(const Config& config);
    const Config& config() const;

    // Start from a known yaw (0.1 degree), at rest
    void reset(uint32_t nowMs, int yaw);
    // The rotate() velocity the yaw servo was told
    void setCommand(int velocity);
    // A sensor reading taken at nowMs (-1: the servo did not answer)
    void update(uint32_t nowMs, int raw);

    int yaw() const;                // unwrapped, 0.1 degree
    float velocity() const;         // 0.1 degree per second
    bool inDeadBand() const;        // the last reading was not taken: outside the range, missing or a glitch
    bool clearOfDeadBand() const;   // the last reading was taken and is edgeMargin steps inside the range
    int takenYaw() const;           // where the last reading taken had the head (unwrapped; no model in it)

private:
    // Where the command takes the head in dt from the current estimate (first order up or down to its rate)
    void predict(float dt, float& yaw, float& velocity) const;
    void coast(float dt);
    void take(uint32_t nowMs, float yaw, int raw);

    Config _config;
    float _yaw                 = 0.0f;
    float _velocity            = 0.0f;
    int _command               = 0;
    uint32_t _last_ms          = 0;
    bool _dead_band            = false;
    bool _in_band              = false;  // the last reading was in the dead band (not a glitch or no answer)
    int _last_raw              = -1;  // the last reading taken
    float _taken_yaw           = 0.0f;
    uint32_t _taken_ms         = 0;
    bool _has_anchor           = false;  // velocity sample start: a reading taken
    float _anchor_yaw          = 0.0f;
    uint32_t _anchor_ms        = 0;
    int _suspects              = 0;  // readings not trusted, in a row, that agree with each other
    float _suspect_yaw         = 0.0f;
    uint32_t _suspect_ms       = 0;
    float _first_suspect       = 0.0f;
    uint32_t _first_suspect_ms = 0;
};

struct SpinStallConfig {
    int minTurn       = 100;   // 0.1 degree ...
    uint32_t windowMs = 1000;  // ... in this long
};

/**
 * Spin scan: is the head driven but not turning (held by a hand, caught by a cable, out of wheel mode)? By the
 * readings the tracker took, the way it is driven: a stuck head's estimate swings to and fro between the command and
 * the readings, which is no progress. Or no reading taken for that long (stuck in the sensor's dead band).
 */
class SpinStallWatch {
public:
    using Config = SpinStallConfig;

    explicit SpinStallWatch(const Config& config = Config());
    void reset(uint32_t nowMs, int takenYaw);
    // Every tick after the tracker's update: velocity is the rotate() command (0: resting on purpose), yawWay the
    // yaw direction it turns the head (+1 / -1). True: stalled.
    bool update(uint32_t nowMs, int velocity, int yawWay, const SpinTracker& tracker);

private:
    Config _config;
    int _ref_yaw         = 0;
    uint32_t _ref_ms     = 0;
    uint32_t _reading_ms = 0;
};

}  // namespace stackpet::radar
