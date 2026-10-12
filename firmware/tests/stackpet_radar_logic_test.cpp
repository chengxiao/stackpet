/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
// Host tests of the RADAR app logic (apps/app_radar/radar_logic.*)
#include "stackpet_games_check.h"
#include <apps/app_radar/radar_logic.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace stackpet::radar;

namespace {

int count_events(const std::vector<RadarEvent>& events, RadarEventType type)
{
    int n = 0;
    for (const auto& e : events) {
        n += e.type == type ? 1 : 0;
    }
    return n;
}

bool entered(const std::vector<RadarEvent>& events, Phase phase)
{
    for (const auto& e : events) {
        if (e.type == RadarEventType::PhaseEntered && e.phase == phase) {
            return true;
        }
    }
    return false;
}

// Drives a machine in 10 ms ticks and collects every event
struct Sim {
    explicit Sim(const RadarConfig& config = RadarConfig()) : machine(config)
    {
    }

    void start(int yaw = 0)
    {
        machine.start(now, yaw, machine.config().scanPitch);
        drain();
    }

    // One tick, optionally with a vision result
    void tick(const Contact* contact = nullptr, bool petted = false)
    {
        now += 10;
        RadarInput input;
        input.nowMs  = now;
        input.petted = petted;
        if (contact != nullptr) {
            input.contact = *contact;
        }
        machine.update(input);
        drain();
    }

    void run(uint32_t ms)
    {
        for (uint32_t t = 0; t < ms; t += 10) {
            tick();
        }
    }

    // A face seen every 100 ms (10 fps detector) for `ms`
    void see(uint32_t ms, float x = 0.0f, float y = 0.0f, float w = 0.15f)
    {
        int hits = 0;
        for (uint32_t t = 0; t < ms; t += 10) {
            if ((now / 10) % 10 == 0) {
                Contact c;
                c.fresh     = true;
                c.present   = true;
                c.hits      = ++hits;
                c.x         = x;
                c.y         = y;
                c.w         = w;
                c.h         = w;
                c.captureMs = now - 60;
                tick(&c);
            } else {
                tick();
            }
        }
    }

    void drain()
    {
        auto events = machine.takeEvents();
        recent.insert(recent.end(), events.begin(), events.end());
        all.insert(all.end(), events.begin(), events.end());
    }

    std::vector<RadarEvent> take()
    {
        std::vector<RadarEvent> out;
        out.swap(recent);
        return out;
    }

    RadarMachine machine;
    uint32_t now = 1000;
    std::vector<RadarEvent> recent;
    std::vector<RadarEvent> all;
};

Contact face_contact(uint32_t captureMs, int hits, float x = 0.0f, float y = 0.0f)
{
    Contact c;
    c.fresh     = true;
    c.present   = true;
    c.hits      = hits;
    c.x         = x;
    c.y         = y;
    c.w         = 0.15f;
    c.h         = 0.2f;
    c.captureMs = captureMs;
    return c;
}

/* -------------------------------------------------------------------------- */

void test_planner_cruise_and_limits()
{
    SweepPlanner::Config config;
    config.limit = 1250;
    config.speed = 300;
    config.accel = 600;
    SweepPlanner planner(config);
    planner.reset(0.0f, 1);

    uint32_t now      = 0;
    float last_v      = 0.0f;
    float max_pos     = -1e9f;
    float min_pos     = 1e9f;
    int reversals     = 0;
    int last_sign     = 0;
    float peak_speed  = 0.0f;
    planner.update(now);
    for (int i = 0; i < 4000; i++) {  // 40 s
        now += 10;
        planner.update(now);
        const float p = planner.position();
        const float v = planner.velocity();
        CHECK(std::fabs(p) <= 1250.0f + 1e-3f);
        CHECK(std::fabs(v) <= 300.0f + 1e-3f);
        // Acceleration never exceeds the configured value (no jerk at the ends)
        CHECK(std::fabs(v - last_v) <= 600.0f * 0.01f + 1e-2f);
        last_v = v;
        max_pos    = std::max(max_pos, p);
        min_pos    = std::min(min_pos, p);
        peak_speed = std::max(peak_speed, std::fabs(v));
        const int sign = v > 0.0f ? 1 : (v < 0.0f ? -1 : 0);
        if (sign != 0) {
            if (last_sign != 0 && sign != last_sign) {
                reversals++;
                // The speed reaches zero right at the end of the sweep
                CHECK(std::fabs(std::fabs(p) - 1250.0f) < 15.0f);
            }
            last_sign = sign;
        }
    }
    CHECK(max_pos > 1240.0f);
    CHECK(min_pos < -1240.0f);
    CHECK_NEAR(peak_speed, 300.0f, 1.0f);
    // 2 x 2500 per cycle at ~30 deg/s plus the turnarounds: ~4.7 reversals in 40 s
    CHECK(reversals >= 4 && reversals <= 6);
}

void test_planner_constant_speed_in_the_middle()
{
    SweepPlanner planner;
    planner.reset(-1000.0f, 1);
    uint32_t now = 0;
    planner.update(now);
    // Accelerate (0.5 s), then cruise
    for (int i = 0; i < 100; i++) {
        now += 10;
        planner.update(now);
    }
    const float p0 = planner.position();
    for (int i = 0; i < 100; i++) {
        now += 10;
        planner.update(now);
    }
    CHECK_NEAR(planner.position() - p0, 300.0f, 1.0f);  // exactly 30 deg in 1 s
    CHECK(planner.direction() == 1);
}

void test_planner_starts_inward_at_a_limit()
{
    SweepPlanner planner;
    planner.reset(1300.0f, 1);  // beyond the limit and heading outwards
    CHECK_NEAR(planner.position(), 1250.0f, 1e-3f);
    uint32_t now = 0;
    planner.update(now);
    for (int i = 0; i < 100; i++) {
        now += 10;
        planner.update(now);
    }
    CHECK(planner.position() < 1200.0f);  // not stuck at the limit
    CHECK(planner.direction() == -1);
}

void test_planner_stall_is_bounded()
{
    SweepPlanner planner;
    planner.reset(0.0f, 1);
    planner.update(0);
    for (uint32_t t = 10; t <= 1000; t += 10) {
        planner.update(t);
    }
    const float p0 = planner.position();
    planner.update(1000 + 5000);  // the main loop hung for 5 s
    CHECK(planner.position() - p0 <= 300.0f * 0.1f + 1.0f);
}

void test_planner_dwell()
{
    SweepPlanner::Config config;
    config.dwellEvery = 400;
    config.dwellMs    = 1000;
    SweepPlanner planner(config);
    planner.reset(-1000.0f, 1);
    uint32_t now = 0;
    planner.update(now);
    bool dwelled    = false;
    uint32_t start  = 0;
    float held      = 0.0f;
    for (int i = 0; i < 1000 && !dwelled; i++) {
        now += 10;
        planner.update(now);
        if (planner.dwelling()) {
            dwelled = true;
            start   = planner.dwellStartMs();
            held    = planner.position();
        }
    }
    CHECK(dwelled);
    CHECK(start == now);
    // Stopped after ~40 deg of travel plus the braking distance
    CHECK(held + 1000.0f >= 400.0f && held + 1000.0f <= 400.0f + 120.0f);
    for (int i = 0; i < 99; i++) {
        now += 10;
        planner.update(now);
        CHECK(planner.dwelling());
        CHECK_NEAR(planner.position(), held, 1e-3f);
    }
    now += 20;
    planner.update(now);
    now += 10;
    planner.update(now);
    CHECK(!planner.dwelling());
    CHECK(planner.position() > held);
}

void test_geometry()
{
    RadarConfig config;
    RadarMachine machine(config);
    // A face at the right edge of a non-mirrored image is on the robot's right: negative yaw
    CHECK(std::abs(machine.yawOffsetFor(1.0f) + 330) <= 1);
    CHECK(std::abs(machine.yawOffsetFor(-1.0f) - 330) <= 1);
    CHECK(machine.yawOffsetFor(0.0f) == 0);
    // tan mapping: half way to the edge is more than half the angle (atan(0.5 * tan 33) = 18.0 degree)
    const int half = machine.yawOffsetFor(0.5f);
    CHECK(half < 0 && std::abs(-half - 180) <= 2);
    // Below the centre: look down
    CHECK(machine.pitchOffsetFor(1.0f) < 0);
    CHECK(std::abs(machine.pitchOffsetFor(1.0f) + 250) <= 1);

    config.imageXToYaw = 1;  // mirrored camera
    RadarMachine mirrored(config);
    CHECK(mirrored.yawOffsetFor(1.0f) > 0);

    CHECK(sweepSpeedForRate(10.0f, 660, 6.0f, 100, 320) == 320);
    CHECK(sweepSpeedForRate(2.0f, 660, 6.0f, 100, 320) == 220);
    CHECK(sweepSpeedForRate(0.5f, 660, 6.0f, 100, 320) == 100);
    CHECK(sweepSpeedForRate(0.0f, 660, 6.0f, 100, 320) == 100);

    CHECK_NEAR(faceDistanceMeters(0.2f, 330), 0.616f, 0.01f);
    CHECK(faceDistanceMeters(0.05f, 330) > 2.0f);
    CHECK(faceDistanceMeters(0.0f, 330) < 20.0f);  // no division by zero
}

void test_scanning_and_pings()
{
    Sim sim;
    sim.start(0);
    CHECK(entered(sim.take(), Phase::Scanning));
    CHECK(sim.machine.phase() == Phase::Scanning);
    CHECK(sim.machine.head().stream);
    CHECK(sim.machine.head().pitch == sim.machine.config().scanPitch);

    sim.run(10000);
    auto events = sim.take();
    // First ping 0.4 s after the start, then every 1.8 s: 0.4, 2.2, 4.0, 5.8, 7.6, 9.4
    CHECK(count_events(events, RadarEventType::Ping) == 6);
    CHECK(sim.machine.phase() == Phase::Scanning);
    // The head streams the sweep
    CHECK(std::abs(sim.machine.head().yaw - static_cast<int>(std::lround(sim.machine.sweepYaw()))) <= 1);
    CHECK(sim.machine.head().yaw != 0);
}

void test_lock_on_needs_two_hits()
{
    Sim sim;
    sim.start(0);
    sim.run(500);
    sim.take();

    Contact first = face_contact(sim.now - 50, 1, 0.4f, 0.0f);
    sim.tick(&first);
    auto events = sim.take();
    CHECK(count_events(events, RadarEventType::Blip) == 1);
    CHECK(sim.machine.phase() == Phase::Scanning);
    for (const auto& e : events) {
        if (e.type == RadarEventType::Blip) {
            CHECK(!e.confirmed);
            CHECK(!e.friendly);
            // Bearing = where the head was + the face offset (right of the image = negative yaw)
            CHECK(e.bearing < sim.machine.yawAt(first.captureMs));
        }
    }

    sim.run(90);
    Contact second         = face_contact(sim.now + 10 - 50, 2, 0.4f, -0.3f);
    const int expected_yaw = sim.machine.yawAt(second.captureMs) + sim.machine.yawOffsetFor(0.4f);
    const int expected_pitch =
        sim.machine.pitchAt(second.captureMs) + sim.machine.pitchOffsetFor(-0.3f);  // face above: look up
    const uint32_t serial = sim.machine.head().serial;
    sim.tick(&second);
    events = sim.take();
    CHECK(entered(events, Phase::Locking));
    CHECK(sim.machine.phase() == Phase::Locking);
    CHECK(!sim.machine.head().stream);
    CHECK(sim.machine.head().serial == serial + 1);
    CHECK(sim.machine.head().yaw == expected_yaw);
    CHECK(sim.machine.head().pitch == expected_pitch);
    CHECK(sim.machine.head().pitch > sim.machine.config().scanPitch);
    CHECK(sim.machine.alarmActive(sim.now));
    CHECK(sim.machine.lockStartMs() == sim.now);
}

void test_full_sequence()
{
    Sim sim;
    const auto& cfg = sim.machine.config();
    sim.start(0);
    sim.run(300);
    sim.see(200, 0.1f);  // two detector runs: locked
    CHECK(sim.machine.phase() == Phase::Locking);
    const uint32_t lock_ms = sim.machine.lockStartMs();

    // The camera view lasts lockViewMs, then the angry face
    for (int i = 0; i < 10000 && sim.machine.phase() == Phase::Locking; i++) {
        sim.see(10, 0.05f);
    }
    CHECK(sim.now - lock_ms >= cfg.lockViewMs && sim.now - lock_ms <= cfg.lockViewMs + 20);
    CHECK(sim.machine.phase() == Phase::Angry);

    // The siren stops after alarmMs
    CHECK(sim.machine.alarmActive(lock_ms + cfg.alarmMs - 1));
    CHECK(!sim.machine.alarmActive(lock_ms + cfg.alarmMs));

    // Stays angry as long as the face stays, well beyond the minimum glare
    sim.see(8000, 0.0f);
    CHECK(sim.machine.phase() == Phase::Angry);

    // The face leaves: calm down after lostMs (glare minimum long over)
    const uint32_t left = sim.now;
    while (sim.machine.phase() == Phase::Angry && sim.now - left < 5000) {
        sim.tick();
    }
    CHECK(sim.machine.phase() == Phase::Cooldown);
    CHECK(sim.now - left >= cfg.lostMs - 100 && sim.now - left <= cfg.lostMs + 20);
    CHECK(!sim.machine.head().stream);
    CHECK(sim.machine.head().pitch == cfg.scanPitch);  // looks up again

    // Scanning again after the cooldown, from where the head is
    const int held_yaw = sim.machine.targetYaw();
    sim.run(cfg.cooldownMs + 20);
    CHECK(sim.machine.phase() == Phase::Scanning);
    CHECK(sim.machine.head().stream);
    CHECK(std::abs(sim.machine.sweepYaw() - held_yaw) < 30.0f);
}

void test_glare_minimum()
{
    Sim sim;
    const auto& cfg = sim.machine.config();
    sim.start(0);
    sim.see(200);
    CHECK(sim.machine.phase() == Phase::Locking);
    // The face disappears at once (false alarm): still the full lock view and the minimum glare
    for (int i = 0; i < 10000 && sim.machine.phase() == Phase::Locking; i++) {
        sim.tick();
    }
    CHECK(sim.machine.phase() == Phase::Angry);
    const uint32_t angry_at = sim.now;
    while (sim.machine.phase() == Phase::Angry && sim.now - angry_at < 10000) {
        sim.tick();
    }
    CHECK(sim.machine.phase() == Phase::Cooldown);
    CHECK(sim.now - angry_at >= cfg.glareMs && sim.now - angry_at <= cfg.glareMs + 20);
}

void test_comes_back_during_cooldown()
{
    Sim sim;
    sim.start(0);
    sim.see(200);
    for (int i = 0; i < 10000 && sim.machine.phase() != Phase::Cooldown; i++) {
        sim.tick();
    }
    CHECK(sim.machine.phase() == Phase::Cooldown);
    const uint32_t lock_ms = sim.machine.lockStartMs();
    sim.run(500);
    sim.take();
    sim.see(200, 0.2f);
    // Angry again right away, without a second lock-on (no second siren)
    CHECK(sim.machine.phase() == Phase::Angry);
    CHECK(!entered(sim.take(), Phase::Locking));
    CHECK(sim.machine.lockStartMs() == lock_ms);
    CHECK(!sim.machine.alarmActive(sim.now));
}

void test_pet_forgives()
{
    Sim sim;
    const auto& cfg = sim.machine.config();
    sim.start(0);
    sim.see(200);
    CHECK(sim.machine.phase() == Phase::Locking);
    sim.run(300);
    CHECK(sim.machine.alarmActive(sim.now));

    const uint32_t pet_ms = sim.now + 10;
    sim.tick(nullptr, true);
    CHECK(sim.machine.phase() == Phase::Forgiven);
    CHECK(!sim.machine.alarmActive(sim.now));  // siren cancelled
    CHECK(sim.machine.inFriendGrace(sim.now));

    sim.run(cfg.forgivenMs);
    CHECK(sim.machine.phase() == Phase::Cooldown);
    // A face during the grace does not make it angry again
    sim.see(300);
    CHECK(sim.machine.phase() == Phase::Cooldown);
    sim.run(cfg.cooldownMs);
    CHECK(sim.machine.phase() == Phase::Scanning);
    sim.take();
    sim.see(300);
    CHECK(sim.machine.phase() == Phase::Scanning);
    bool friendly_blip = false;
    for (const auto& e : sim.take()) {
        if (e.type == RadarEventType::Blip) {
            friendly_blip = e.friendly;
        }
    }
    CHECK(friendly_blip);

    // The grace runs from the last sighting (within the last 100 ms), not from the pet: still on friendGraceMs after
    // the pet
    CHECK(sim.now - pet_ms < cfg.friendGraceMs);
    sim.run(cfg.friendGraceMs - 300);
    CHECK(sim.now - pet_ms > cfg.friendGraceMs);
    CHECK(sim.machine.inFriendGrace(sim.now));

    // Nobody seen for the whole grace: an intruder again
    sim.run(400);
    CHECK(!sim.machine.inFriendGrace(sim.now));
    sim.see(200);
    CHECK(sim.machine.phase() == Phase::Locking);
}

void test_friend_grace_renews_while_seen()
{
    // The person who petted the head stays at the desk and is swept past every few seconds: no second siren
    Sim sim;
    const auto& cfg = sim.machine.config();
    sim.start(0);
    sim.see(200);
    CHECK(sim.machine.phase() == Phase::Locking);
    sim.tick(nullptr, true);
    CHECK(sim.machine.phase() == Phase::Forgiven);
    const uint32_t pet_ms = sim.now;
    sim.take();

    for (int i = 0; i < 10; i++) {  // 10 x (0.3 s seen + 5 s not) = 53 s, well past one grace
        sim.see(300);
        sim.run(5000);
    }
    CHECK(sim.now - pet_ms > cfg.friendGraceMs);
    CHECK(sim.machine.inFriendGrace(sim.now));
    CHECK(sim.machine.phase() == Phase::Scanning);
    CHECK(!entered(sim.take(), Phase::Locking));

    // Gone for the whole grace: an intruder again
    sim.run(cfg.friendGraceMs);
    CHECK(!sim.machine.inFriendGrace(sim.now));
    sim.see(200);
    CHECK(sim.machine.phase() == Phase::Locking);
}

void test_fine_tracking()
{
    Sim sim;
    const auto& cfg = sim.machine.config();
    sim.start(0);
    sim.see(200, 0.0f);
    CHECK(sim.machine.phase() == Phase::Locking);
    const int yaw0   = sim.machine.targetYaw();
    const int pitch0 = sim.machine.targetPitch();
    uint32_t serial  = sim.machine.head().serial;

    // A frame captured while the head was still turning is ignored
    Contact early = face_contact(sim.now, 5, 0.5f, 0.0f);
    sim.tick(&early);
    CHECK(sim.machine.head().serial == serial);

    // A settled frame with the face right of centre: turn right by gain x error
    sim.run(cfg.settleMs + 20);
    Contact settled = face_contact(sim.now, 6, 0.3f, 0.2f);
    const int err_yaw   = sim.machine.yawOffsetFor(0.3f);
    const int err_pitch = sim.machine.pitchOffsetFor(0.2f);
    sim.tick(&settled);
    CHECK(sim.machine.head().serial == serial + 1);
    CHECK(std::abs(sim.machine.head().yaw - (yaw0 + static_cast<int>(std::lround(err_yaw * cfg.trackGain)))) <= 1);
    CHECK(std::abs(sim.machine.head().pitch - (pitch0 + static_cast<int>(std::lround(err_pitch * cfg.trackGain)))) <= 1);
    CHECK(sim.machine.head().speed == cfg.trackSpeed);
    serial = sim.machine.head().serial;

    // Within the dead band: no correction
    sim.run(cfg.settleMs + 20);
    Contact centred = face_contact(sim.now, 7, 0.02f, -0.02f);
    sim.tick(&centred);
    CHECK(sim.machine.head().serial == serial);
}

void test_aim_is_clamped()
{
    Sim sim;
    sim.start(1240);  // near the left limit
    sim.run(100);
    // A face at the far left edge of the image: beyond the servo range
    Contact a = face_contact(sim.now, 1, -1.0f, -1.0f);
    sim.tick(&a);
    Contact b = face_contact(sim.now, 2, -1.0f, -1.0f);
    sim.tick(&b);
    CHECK(sim.machine.phase() == Phase::Locking);
    CHECK(sim.machine.head().yaw <= sim.machine.config().aimYawLimit);
    CHECK(sim.machine.head().pitch <= sim.machine.config().pitchMax);
}

void test_yaw_history_follows_the_sweep()
{
    Sim sim;
    const auto& cfg = sim.machine.config();
    sim.start(-1200);
    std::vector<std::pair<uint32_t, float>> samples;
    for (int i = 0; i < 300; i++) {
        sim.tick();
        samples.push_back({sim.now, sim.machine.sweepYaw()});
    }
    // A frame captured at t shows the head where the sweep was servoLagMs earlier
    for (size_t i = 150; i < samples.size(); i += 17) {
        const uint32_t t = samples[i].first;
        float expected   = 0.0f;
        for (const auto& s : samples) {
            if (s.first == t - static_cast<uint32_t>(cfg.servoLagMs)) {
                expected = s.second;
            }
        }
        CHECK(std::fabs(sim.machine.yawAt(t) - expected) <= 4.0f);
    }
}

/* -------------------------------------------------------------------------- */
/*                                  Spin scan                                 */
/* -------------------------------------------------------------------------- */
// A spinning head (wheel mode) next to the machine: while head().rotate is set it turns at kSpinRate per velocity
// unit (the CoreS3: about 0.44 degree/s per unit, rotate(+v) turns towards -yaw); otherwise it jumps to head().yaw.
struct SpinSim {
    static constexpr float kSpinRate = 4.4f;  // 0.1 degree per second per rotate() unit

    explicit SpinSim(const RadarConfig& config) : machine(config)
    {
    }
    void start(int yaw = 0)
    {
        head_yaw = static_cast<float>(yaw);
        machine.start(now, yaw, machine.config().scanPitch);
        drain();
    }
    void tick(const Contact* contact = nullptr)
    {
        now += 10;
        const auto& head = machine.head();
        if (head.rotate) {
            head_yaw += -static_cast<float>(head.velocity) * kSpinRate * 0.01f;
        } else {
            head_yaw = static_cast<float>(head.yaw);
        }
        RadarInput input;
        input.nowMs = now;
        // Measured while the yaw servo is in wheel mode (the app reports none once the head is back in position mode)
        input.headValid    = head.rotate || escaping;
        input.headYaw      = static_cast<int>(std::lround(head_yaw));
        input.headVelocity = head.rotate ? -static_cast<float>(head.velocity) * kSpinRate : 0.0f;
        if (contact != nullptr) {
            input.contact = *contact;
        }
        machine.update(input);
        drain();
        const auto& next = machine.head();
        if (next.rotate && next.brakeArmed && next.brakeSerial != brake_serial) {
            // The app turns on to the brake point before it stops (the sim's sensor is exact)
            brake_serial  = next.brakeSerial;
            const int way = next.brakeVelocity > 0 ? -1 : 1;
            if ((static_cast<float>(next.brakeAtYaw) - head_yaw) * static_cast<float>(way) > 0.0f) {
                head_yaw = static_cast<float>(next.brakeAtYaw);
            }
        }
    }
    void run(uint32_t ms)
    {
        for (uint32_t t = 0; t < ms; t += 10) {
            tick();
        }
    }
    // A still frame result with the face at x (captured `ageMs` ago)
    void see(float x = 0.0f, uint32_t ageMs = 60)
    {
        Contact c = face_contact(now - ageMs, 1, x);
        tick(&c);
    }
    void none(uint32_t ageMs = 60)
    {
        Contact c;
        c.fresh     = true;
        c.present   = false;
        c.captureMs = now - ageMs;
        tick(&c);
    }
    void drain()
    {
        auto events = machine.takeEvents();
        all.insert(all.end(), events.begin(), events.end());
    }
    RadarMachine machine;
    uint32_t now   = 1000;
    float head_yaw = 0.0f;
    bool escaping  = false;  // the app still leaves the sensor's dead band in wheel mode
    uint32_t brake_serial = 0;
    std::vector<RadarEvent> all;
};

RadarConfig spin_config()
{
    RadarConfig cfg;
    cfg.spinScan       = true;
    cfg.halfFovYaw     = 261;
    cfg.imageXToYaw    = 1;
    cfg.frameLatencyMs = 40;
    return cfg;
}

// Spin on until a face in the image centre is at about `bearing` (exposure latency included), then show it
bool face_at_bearing(SpinSim& sim, int bearing)
{
    const float rate = 3960.0f;  // 0.1 degree/s at rotate(900) in the sim
    for (int i = 0; i < 4000; i++) {
        const auto& head = sim.machine.head();
        const int seen   = wrapYaw(static_cast<int>(std::lround(sim.head_yaw - 0.05f * rate)));
        if (head.rotate && head.velocity != 0 && std::abs(wrapYaw(seen - bearing)) <= 60) {
            Contact c = face_contact(sim.now, 1, 0.0f);
            sim.tick(&c);
            return true;
        }
        sim.tick();
    }
    return false;
}


// The CoreS3's yaw servo in wheel mode, for the SpinTracker: a head with inertia (first order: speeding up with
// tauMs, slowing down with slowTauMs, PWM 0 freewheels) turning towards -yaw at ratePerUnit per rotate() unit, and its
// position sensor: 0 .. 1000 over 312.5 degree (zero at 460), then the dead band, where it reads 1002 .. 1022: 47.5
// degree in the readings' terms, but only about 42 of the head's turn (it crosses it 1.15 times as fast)
struct WheelHead {
    float yaw         = 0.0f;  // unwrapped, 0.1 degree
    float rate        = 0.0f;  // 0.1 degree per second
    float ratePerUnit = 4.6f;
    float tauMs       = 30.0f;
    float slowTauMs   = 55.0f;
    float bandRateScale = 1.15f;
    bool held         = false;  // by a hand (or a cable): it does not turn

    void run(int command, uint32_t ms)
    {
        for (uint32_t i = 0; i < ms; i++) {
            const float target = -static_cast<float>(command) * ratePerUnit;
            const bool slowing = std::fabs(target) < std::fabs(rate) || target * rate < 0.0f;
            rate += (target - rate) * (1.0f - std::exp(-1.0f / (slowing ? slowTauMs : tauMs)));
            if (held) {
                rate = 0.0f;
            }
            yaw += rate * 0.001f * (step() > 1000 ? bandRateScale : 1.0f);
        }
    }
    int step() const  // 0 .. 1151 over a turn, above 1000 in the dead band
    {
        const float raw = std::fmod(460.0f + yaw / 3.125f, 1152.0f);
        return static_cast<int>(std::floor(raw < 0.0f ? raw + 1152.0f : raw));
    }
    int read() const
    {
        const int raw = step();
        return raw > 1000 ? 1002 + raw % 21 : raw;
    }
};

void test_spin_tracker()
{
    // Turning towards +yaw at rotate(-800) through the dead band three times, the head 10 % slower than the
    // tracker's model of it: no jumps, close all the time
    {
        WheelHead head;
        head.ratePerUnit = 4.6f * 0.9f;
        SpinTracker tracker;  // zero 460
        tracker.reset(0, 0);
        uint32_t t        = 0;
        int last          = 0;
        int blind         = 0;
        float worst       = 0.0f;
        for (int i = 0; i < 150; i++) {  // 3 s
            head.run(-800, 20);
            t += 20;
            tracker.setCommand(-800);
            tracker.update(t, head.read());
            blind += tracker.inDeadBand() ? 1 : 0;
            CHECK(tracker.yaw() >= last - 10);   // never back
            CHECK(tracker.yaw() - last <= 150);  // nor ahead: 66 per 20 ms
            last  = tracker.yaw();
            worst = std::max(worst, std::fabs(static_cast<float>(tracker.yaw()) - head.yaw));
        }
        CHECK(blind >= 15);
        CHECK(worst < 60.0f);
        CHECK_NEAR(tracker.velocity(), head.rate, 300.0f);
    }

    // Told to stop in the dead band: the estimate stops with the head (no reading says where). Driven again, it
    // goes on and picks up the readings after the band.
    {
        WheelHead head;
        head.slowTauMs = 70.0f;  // coasts on further than the tracker's model has it
        SpinTracker tracker;
        uint32_t t = 0;
        tracker.reset(t, 0);
        tracker.setCommand(-800);
        while (head.yaw < 1800.0f) {  // 11 degree into the dead band (it starts at 1687.5)
            head.run(-800, 20);
            t += 20;
            tracker.update(t, head.read());
        }
        CHECK(tracker.inDeadBand());
        tracker.setCommand(0);
        for (int i = 0; i < 25; i++) {  // 500 ms
            head.run(0, 20);
            t += 20;
            tracker.update(t, head.read());
        }
        CHECK(head.step() > 1000);  // came to rest in the dead band
        CHECK(std::fabs(static_cast<float>(tracker.yaw()) - head.yaw) < 80.0f);
        const int resting = tracker.yaw();
        for (int i = 0; i < 25; i++) {  // nothing changes while it rests
            t += 20;
            tracker.update(t, head.read());
        }
        CHECK(tracker.yaw() == resting);
        CHECK(std::fabs(tracker.velocity()) < 1.0f);
        tracker.setCommand(-700);
        while (head.step() > 1000 || head.step() < 30) {
            head.run(-700, 20);
            t += 20;
            tracker.update(t, head.read());
        }
        head.run(-700, 20);
        t += 20;
        tracker.update(t, head.read());
        CHECK(!tracker.inDeadBand());
        CHECK(std::fabs(static_cast<float>(tracker.yaw()) - head.yaw) < 20.0f);
    }

    // Glitches next to the dead band: one between dead band values while driven, or 801 again and again while the
    // head rests at its edge, are not taken
    {
        WheelHead head;
        head.yaw = 1600.0f;
        SpinTracker tracker;
        uint32_t t = 0;
        tracker.reset(t, 1600);
        tracker.setCommand(-800);
        bool glitched = false;
        for (int i = 0; i < 40; i++) {
            head.run(-800, 20);
            t += 20;
            int raw = head.read();
            if (!glitched && raw <= 1000 && raw < 100 && i > 3) {
                raw      = 221;  // the first reading after the band (seen on the CoreS3)
                glitched = true;
            }
            tracker.update(t, raw);
            CHECK(std::fabs(static_cast<float>(tracker.yaw()) - head.yaw) < 60.0f);
        }
        CHECK(glitched);

        SpinTracker edge;
        edge.reset(0, 1675);  // reading 996
        edge.setCommand(0);
        for (uint32_t i = 1; i <= 6; i++) {
            edge.update(i * 20, i % 3 == 0 ? 1012 : 801);
            CHECK(std::abs(edge.yaw() - 1675) <= 2);
        }
        edge.update(140, 996);
        CHECK(!edge.inDeadBand());
        CHECK(!edge.clearOfDeadBand());  // 4 steps from the band: glitches come there
        CHECK(std::abs(edge.yaw() - 1675) <= 2);
    }

    // Held while driven: the readings stand still, and after confirmHoldMs so does the estimate (the command alone
    // would run it on; the app notices the stall by the readings and backs off)
    {
        WheelHead head;
        head.yaw = -1000.0f;
        SpinTracker tracker;
        uint32_t t = 0;
        tracker.reset(t, -1000);
        tracker.setCommand(-800);
        for (int i = 0; i < 10; i++) {
            head.run(-800, 20);
            t += 20;
            tracker.update(t, head.read());
        }
        head.held    = true;
        int held_yaw = 0;
        for (int i = 0; i < 40; i++) {
            head.run(-800, 20);
            t += 20;
            tracker.update(t, head.read());
            if (i == 19) {
                held_yaw = tracker.yaw();
            }
        }
        CHECK(std::fabs(static_cast<float>(tracker.yaw()) - head.yaw) < 10.0f);
        CHECK(std::abs(tracker.yaw() - held_yaw) < 10);
        CHECK(std::fabs(static_cast<float>(tracker.takenYaw()) - head.yaw) < 10.0f);  // no progress by the readings
        CHECK(tracker.clearOfDeadBand());
    }

    // No answer (-1) is like the dead band: the command goes on
    {
        SpinTracker tracker;
        tracker.reset(0, 0);
        tracker.update(20, 460);
        tracker.update(40, -1);
        CHECK(tracker.inDeadBand());
        CHECK(tracker.yaw() == 0);  // told nothing: at rest
        tracker.setCommand(-800);
        for (uint32_t t = 60; t <= 400; t += 20) {
            tracker.update(t, -1);
        }
        CHECK_NEAR(tracker.velocity(), 3680.0f, 1.0f);
        CHECK(tracker.yaw() > 1000 && tracker.yaw() < 1300);  // 0.36 s at up to 368 degree/s
    }

    // Readings around the top and the bottom of the range continue the same turn
    SpinTracker up;
    up.reset(0, 1600);
    up.update(20, 970);  // (970 - 460) * 3.125 = 1593.75
    CHECK(std::abs(up.yaw() - 1594) <= 1);
    up.update(40, 500);
    CHECK(up.clearOfDeadBand() == false);  // too far from 1594 to be taken at rest
    CHECK(wrapYaw(1800) == -1800);
    CHECK(wrapYaw(-1801) == 1799);
    CHECK(wrapYaw(7300) == 100);
}

// The spin scan in closed loop: RadarMachine -> rotate() command -> a head with inertia -> its sensor (dead band and
// its noisy values, glitches next to it, some repeating, missed answers) -> SpinTracker -> RadarMachine, on uneven
// ticks. Records where each look-around stop had the head (where it came to rest).
struct SpinLoop {
    std::vector<float> stops;  // where the head rested at each look-around stop (unwrapped)
    std::vector<int> bands;    // ... its sensor step there (above 1000: the dead band)
    std::vector<float> fast;   // turn of the head in each fast stage
    float worst_error = 0.0f;  // |estimate - head| where a stop ends
    int checks        = 0;
    int stalls        = 0;     // SpinStallWatch saying the turning head does not turn
};

SpinLoop spin_closed_loop(float startYaw, float rateScale, float tauMs, int glitchPermille, uint32_t tickMinMs,
                          uint32_t tickMaxMs, uint32_t seed, float slowTauMs = 55.0f)
{
    uint32_t rng = seed * 2654435761u + 1;
    auto roll    = [&rng](uint32_t n) {
        rng = rng * 1664525u + 1013904223u;
        return (rng >> 8) % n;
    };
    auto cfg         = spin_config();
    cfg.spinVelocity = 800;
    RadarMachine machine(cfg);
    WheelHead head;
    head.yaw         = startYaw;
    head.ratePerUnit = 4.6f * rateScale;
    head.tauMs       = tauMs;
    head.slowTauMs   = slowTauMs;
    SpinTracker tracker;
    SpinStallWatch stall;
    uint32_t now = 1000;
    tracker.reset(now, static_cast<int>(std::lround(startYaw)));
    stall.reset(now, tracker.takenYaw());
    machine.start(now, static_cast<int>(std::lround(startYaw)), cfg.scanPitch);
    machine.takeEvents();

    SpinLoop loop;
    uint32_t next_frame   = now + 100;
    int last_glitch       = 801;
    uint32_t brake_serial = 0;
    auto read_sensor    = [&]() {
        int raw         = head.read();
        const int steps = head.step();
        if ((steps > 985 || steps < 15) && static_cast<int>(roll(1000)) < glitchPermille) {
            const uint32_t kind = roll(3);  // 221 / 801 (seen on the CoreS3), the last one again, anything
            raw         = kind == 0 ? (roll(2) != 0 ? 221 : 801) : (kind == 1 ? last_glitch : static_cast<int>(roll(1001)));
            last_glitch = raw;
        } else if (roll(1000) < 5) {
            raw = -1;
        }
        return raw;
    };
    bool looking        = false;
    bool resting        = false;
    float fast_from     = head.yaw;
    int turns           = 0;
    while (turns < 2 && now < 120000) {
        const auto& cmd   = machine.head();
        const int command = cmd.rotate ? cmd.velocity : 0;
        uint32_t dt = 0;
        if (tickMinMs > 0) {
            dt = tickMinMs + roll(tickMaxMs - tickMinMs + 1);
        } else {
            // The CoreS3's main loop (3029 intervals in cal_session.log): median 19 ms, p90 27, 3.8 % 40 or more, up
            // to 86
            const uint32_t r = roll(100);
            dt = r < 85 ? 15 + roll(11) : (r < 96 ? 25 + roll(16) : 40 + roll(47));
        }
        head.run(command, dt);
        now += dt;
        tracker.setCommand(command);
        tracker.update(now, read_sensor());
        if (stall.update(now, command, command > 0 ? -1 : 1, tracker)) {
            loop.stalls++;
            stall.reset(now, tracker.takenYaw());
        }

        RadarInput input;
        input.nowMs        = now;
        input.headValid    = true;
        input.headYaw      = tracker.yaw();
        input.headVelocity = tracker.velocity();
        if (static_cast<int32_t>(now - next_frame) >= 0) {
            next_frame += 100;
            input.contact.fresh     = true;
            input.contact.present   = false;
            input.contact.captureMs = now - 60;
        }
        machine.update(input);
        loop.checks += entered(machine.takeEvents(), Phase::Checking) ? 1 : 0;
        const auto& next       = machine.head();
        const bool new_brake   = next.rotate && next.brakeArmed && next.brakeSerial != brake_serial;
        const bool now_looking = machine.lookingAround();
        if (now_looking && !looking) {
            loop.fast.push_back(head.yaw - fast_from);
        } else if (!now_looking && looking) {
            fast_from = head.yaw;
            turns++;
        }
        // A stop ends when the head is told to move on (or braked on to the next one from rest)
        if (resting && (new_brake || next.velocity != 0 || !now_looking)) {
            loop.stops.push_back(head.yaw);
            loop.bands.push_back(head.step());
            loop.worst_error = std::max(loop.worst_error, std::fabs(static_cast<float>(tracker.yaw()) - head.yaw));
        }
        resting = now_looking && next.rotate && next.velocity == 0;
        looking = now_looking;
        if (new_brake) {
            // The app: on (from rest too) until the sensor says the head is at the brake point (a reading every ms)
            brake_serial  = next.brakeSerial;
            const int way = next.brakeVelocity > 0 ? -1 : 1;
            tracker.setCommand(next.brakeVelocity);
            for (int ms = 0; ms < 200 && (next.brakeAtYaw - tracker.yaw()) * way > 0; ms++) {
                head.run(next.brakeVelocity, 1);
                now++;
                tracker.update(now, read_sensor());
            }
        }
    }
    return loop;
}

void test_spin_scan_closed_loop()
{
    struct Case {
        float rateScale;
        float tauMs;
        float slowTauMs;
        int glitchPermille;
        uint32_t tickMinMs;
        uint32_t tickMaxMs;
    };
    // The CoreS3's head (it freewheels about 17 degree from 700, as the brake allows for) on steady ticks; then on
    // ticks of any length from 10 to 40 ms: the same, 12 % slower or faster, half or twice as sluggish speeding up,
    // freewheeling a quarter less or further, glitches 3 or 10 % of the readings next to the dead band (CoreS3: 15
    // of 534)
    const Case cases[] = {{1.0f, 30.0f, 55.0f, 30, 20, 30},  {1.0f, 30.0f, 55.0f, 30, 10, 40},
                          {0.88f, 30.0f, 55.0f, 30, 10, 40}, {1.12f, 30.0f, 55.0f, 30, 10, 40},
                          {1.0f, 15.0f, 55.0f, 30, 10, 40},  {1.0f, 60.0f, 55.0f, 30, 10, 40},
                          {1.0f, 30.0f, 40.0f, 30, 10, 40},  {1.0f, 30.0f, 70.0f, 30, 10, 40},
                          {1.0f, 30.0f, 55.0f, 100, 10, 40}};
    const RadarMachine stops(spin_config());
    const int gap      = stops.lookStopYaw(1) - stops.lookStopYaw(0);
    const int band_gap = 3600 - (stops.lookStopYaw(7) - stops.lookStopYaw(0));
    CHECK(gap == 417 && band_gap == 676);
    int runs = 0;
    for (const auto& c : cases) {
        // Where a stop ends up: the coast the brake does not allow for (it brakes lookCoastMs worth before, at the
        // speed it should be going at least), the estimate (the app brakes where the sensor says, read every ms in
        // between ticks)
        const float rate      = 700.0f * 4.6f * c.rateScale / 1000.0f;  // 0.1 degree per ms
        const float braked    = std::max(rate, 3.22f) * static_cast<float>(spin_config().lookCoastMs);
        // ... and the stop after the dead band brakes inside it (about 40 degree in), where only the model has the head:
        // a head faster or slower, or quicker or slower to speed up, than the model is off there
        const float tolerance = std::fabs(rate * c.slowTauMs - braked) + std::fabs(c.rateScale - 1.0f) * 400.0f +
                                std::fabs(c.tauMs - 30.0f) * rate + 60.0f;
        for (int start = -1800; start < 1800; start += 150) {
            const auto loop = spin_closed_loop(static_cast<float>(start), c.rateScale, c.tauMs, c.glitchPermille,
                                               c.tickMinMs, c.tickMaxMs, static_cast<uint32_t>(start + 2000 + runs),
                                               c.slowTauMs);
            runs++;
            CHECK(loop.checks == 0);
            CHECK(loop.stalls == 0);
            CHECK(loop.fast.size() >= 2);
            for (const float f : loop.fast) {  // two turns
                CHECK(f > 6900.0f && f < 7500.0f);
            }
            // Each look-around turn rests once at every stop bearing, in order (the spin's way round)
            CHECK(loop.stops.size() == 16);
            for (size_t i = 0; i < loop.stops.size(); i++) {
                const int at = static_cast<int>(std::lround(loop.stops[i]));
                int nearest  = 0;
                for (int k = 1; k < 8; k++) {
                    if (std::abs(wrapYaw(at - stops.lookStopYaw(k))) < std::abs(wrapYaw(at - stops.lookStopYaw(nearest)))) {
                        nearest = k;
                    }
                }
                CHECK(std::abs(wrapYaw(at - stops.lookStopYaw(nearest))) <= tolerance);
                if (i % 8 != 0) {
                    // From the stop before: 42 degree, 68 over the dead band
                    const float step   = loop.stops[i] - loop.stops[i - 1];
                    const float spread = 2.0f * 60.0f;
                    CHECK(step > static_cast<float>(gap) - spread && step < static_cast<float>(band_gap) + spread);
                }
                if (c.tickMaxMs <= 30) {
                    CHECK(loop.bands[i] <= 1000);  // the CoreS3: never at rest in the dead band
                }
            }
            CHECK(loop.worst_error < 150.0f);
        }
    }
    CHECK(runs == 216);
}

void test_spin_stall_watch()
{
    // Held while driven, on short and long ticks (the main loop can take 30 .. 90 ms), where the sensor reads and in
    // its dead band: noticed within 1.5 s, not before (and never while it turns: the closed loop counts none)
    const uint32_t ticks[][2] = {{20, 30}, {30, 45}, {40, 60}, {50, 90}};
    uint32_t rng              = 12345;
    for (const auto& tick : ticks) {
        for (const float hold_at : {500.0f, 1900.0f}) {  // the dead band is 1687.5 .. 2162.5
            WheelHead head;
            head.yaw = hold_at - 600.0f;  // spun up to it first
            SpinTracker tracker;
            SpinStallWatch watch;
            uint32_t now = 1000;
            tracker.reset(now, static_cast<int>(std::lround(head.yaw)));
            watch.reset(now, tracker.takenYaw());
            uint32_t held_from = 0;
            uint32_t noticed   = 0;
            while (now < 8000 && noticed == 0) {
                rng = rng * 1664525u + 1013904223u;
                const uint32_t dt = tick[0] + (rng >> 8) % (tick[1] - tick[0] + 1);
                head.run(-800, dt);
                now += dt;
                if (!head.held && head.yaw >= hold_at) {
                    head.held = true;
                    held_from = now;
                }
                tracker.setCommand(-800);
                tracker.update(now, head.read());
                if (watch.update(now, -800, 1, tracker)) {
                    noticed = now;
                }
            }
            CHECK(held_from != 0);
            CHECK(noticed > held_from);
            CHECK(noticed - held_from <= 1500);
        }
    }
    // Resting on purpose is no stall
    SpinTracker tracker;
    SpinStallWatch watch;
    tracker.reset(0, 0);
    watch.reset(0, 0);
    for (uint32_t t = 20; t < 5000; t += 20) {
        tracker.update(t, 460);
        CHECK(!watch.update(t, 0, 1, tracker));
    }
}

void test_spin_scan_closed_loop_real_ticks()
{
    // The CoreS3's head on the CoreS3's main loop ticks: the app watches the sensor for the brake point in between
    // ticks, so long ticks do not carry the stops past their bearings
    const RadarMachine stops(spin_config());
    int total = 0, within_5 = 0, within_10 = 0, in_band = 0, worst = 0;
    for (int start = -1800; start < 1800; start += 75) {
        for (uint32_t seed = 1; seed <= 3; seed++) {
            const auto loop = spin_closed_loop(static_cast<float>(start), 1.0f, 30.0f, 30, 0, 0,
                                               static_cast<uint32_t>(start + 9000) * 7u + seed);
            CHECK(loop.checks == 0);
            CHECK(loop.stalls == 0);
            CHECK(loop.stops.size() == 16);
            for (size_t i = 0; i < loop.stops.size(); i++) {
                const int at = static_cast<int>(std::lround(loop.stops[i]));
                int off      = 3600;
                for (int k = 0; k < 8; k++) {
                    off = std::min(off, std::abs(wrapYaw(at - stops.lookStopYaw(k))));
                }
                total++;
                within_5 += off <= 50 ? 1 : 0;
                within_10 += off <= 100 ? 1 : 0;
                in_band += loop.bands[i] > 1000 ? 1 : 0;
                worst = std::max(worst, off);
            }
        }
    }
    std::cout << "  real ticks: " << total << " stops, " << within_5 * 100.0 / total << " % within 5 degree, "
              << within_10 * 100.0 / total << " % within 10, " << in_band << " in the dead band, worst "
              << worst / 10.0 << " degree\n";
    CHECK(total == 48 * 3 * 16);
    // Measured 99.8 % / 100 % / none / 6.3 degree with the brake point watched in between ticks (braking on the
    // ticks alone: 81 % / 97.5 % / 0.4 % / 19 degree)
    CHECK(within_5 * 100 >= total * 99);
    CHECK(within_10 == total);
    CHECK(in_band == 0);
    CHECK(worst <= 100);
}

void test_spin_scan_fast_turns_then_look_around()
{
    auto cfg = spin_config();
    SpinSim sim(cfg);
    sim.start(0);
    CHECK(sim.machine.phase() == Phase::Scanning);
    sim.tick();
    CHECK(sim.machine.head().rotate);
    CHECK(sim.machine.spinDirection() == 1);
    CHECK(sim.machine.head().velocity == -900);  // towards +yaw: rotate(-v) on the CoreS3
    CHECK(sim.machine.head().pitch == cfg.scanPitch);
    CHECK(!sim.machine.lookingAround());
    sim.run(500);
    CHECK(count_events(sim.all, RadarEventType::Ping) >= 1);  // pings go on while spinning
    // The radar line follows the measured yaw, wrapped
    CHECK(std::abs(sim.machine.sweepYaw() - wrapYaw(static_cast<int>(std::lround(sim.head_yaw)))) <= 1.0f);

    // Two fast turns, then the look-around turn
    int ticks = 0;
    while (!sim.machine.lookingAround() && ticks++ < 400) {
        sim.tick();
    }
    CHECK(sim.machine.lookingAround());
    CHECK(std::abs(sim.head_yaw - 7200.0f) < 120.0f);
    CHECK(sim.machine.head().velocity == -cfg.lookVelocity);

    // Eight stops at the fixed bearings (from where the sensor reads to where it stops reading, 42 degree apart),
    // the same way round; the camera gets still frames at each
    CHECK(sim.machine.lookStopYaw(0) == cfg.lookFromYaw);
    CHECK(sim.machine.lookStopYaw(7) == cfg.lookToYaw);
    for (int k = 0; k < 7; k++) {
        const int gap = sim.machine.lookStopYaw(k + 1) - sim.machine.lookStopYaw(k);
        CHECK(gap >= 417 && gap <= 418);
    }
    const float look_start = sim.head_yaw;
    std::vector<float> stops;
    bool stopped = false;
    ticks        = 0;
    while (sim.machine.lookingAround() && ticks++ < 3000) {
        const bool at_rest = sim.machine.head().rotate && sim.machine.head().velocity == 0;
        if (at_rest && !stopped) {
            stops.push_back(sim.head_yaw);
        }
        stopped = at_rest;
        if (at_rest && ticks % 10 == 0) {
            sim.none(30);  // a still frame without a face
        } else {
            sim.tick();
        }
        CHECK(sim.machine.spinDirection() == 1);
    }
    CHECK(stops.size() == 8);
    // It brakes lookCoastMs worth before each stop, at the speed the head should be going at least
    const int coast = static_cast<int>(std::lround(cfg.lookVelocity * cfg.spinRatePerUnit * cfg.lookCoastMs / 1000.0f));
    CHECK(stops[0] - look_start >= cfg.lookLeadYaw - coast);  // room to brake for the first one
    std::vector<bool> seen(8, false);
    for (size_t i = 0; i < stops.size(); i++) {
        if (i > 0) {
            CHECK(stops[i] > stops[i - 1]);
        }
        // The sim's head stops dead at the brake point
        const int at = static_cast<int>(std::lround(stops[i])) + coast;
        for (int k = 0; k < 8; k++) {
            if (std::abs(wrapYaw(at - sim.machine.lookStopYaw(k))) <= 2) {
                seen[k] = true;
            }
        }
    }
    CHECK(std::all_of(seen.begin(), seen.end(), [](bool b) { return b; }));
    // Then fast turns again, still the same way
    CHECK(sim.machine.phase() == Phase::Scanning);
    CHECK(!sim.machine.lookingAround());
    CHECK(sim.machine.head().velocity == -900);
}

void test_spin_scan_look_around_finds_a_face()
{
    auto cfg = spin_config();
    SpinSim sim(cfg);
    sim.start(0);
    int ticks = 0;
    while (!(sim.machine.lookingAround() && sim.machine.head().velocity == 0) && ticks++ < 600) {
        sim.tick();
    }
    CHECK(sim.machine.head().velocity == 0);
    sim.run(cfg.lookSettleMs + 20);
    // A still frame with a face on the left: turn to it (the head stands, the bearing is exact) and check
    const float head = sim.head_yaw;
    sim.see(-0.4f, 10);
    CHECK(sim.machine.phase() == Phase::Checking);
    const int expected = std::clamp(wrapYaw(static_cast<int>(std::lround(head)) + sim.machine.yawOffsetFor(-0.4f)),
                                    -cfg.aimYawLimit, cfg.aimYawLimit);
    CHECK(std::abs(sim.machine.targetYaw() - expected) <= 5);
}

void test_spin_scan_heat_pause()
{
    auto cfg = spin_config();
    SpinSim sim(cfg);
    sim.start(0);
    sim.run(100);
    sim.machine.setSpinVelocity(0);  // the servo got hot
    sim.tick();
    CHECK(sim.machine.head().rotate);
    CHECK(sim.machine.head().velocity == 0);
    const float yaw = sim.head_yaw;
    sim.run(3000);
    CHECK(sim.head_yaw == yaw);
    sim.machine.setSpinVelocity(500);  // warm: slower
    sim.tick();
    CHECK(sim.machine.head().velocity == -500);
    sim.machine.setSpinVelocity(900);
    sim.tick();
    CHECK(sim.machine.head().velocity == -900);
}

void test_spin_scan_brakes_turns_back_and_locks()
{
    auto cfg = spin_config();
    SpinSim sim(cfg);
    sim.start(0);
    sim.run(200);  // spinning, about 80 degree on (the face stays inside the aim limits)
    CHECK(sim.machine.head().rotate);

    // A (blurred) frame taken 150 ms ago, face right of the centre: where was the head then?
    const uint32_t capture = sim.now - 150;
    const float yaw_at     = sim.head_yaw - 3960.0f * 0.19f;  // 150 ms + the 40 ms exposure latency earlier
    Contact blurred        = face_contact(capture, 1, 0.5f);
    sim.tick(&blurred);
    CHECK(sim.machine.phase() == Phase::Checking);
    CHECK(!sim.machine.head().rotate);
    const int offset   = sim.machine.yawOffsetFor(0.5f);
    CHECK(offset > 0);  // imageXToYaw +1: a face on the right is towards +yaw
    const int expected = std::clamp(wrapYaw(static_cast<int>(std::lround(yaw_at)) + offset), -cfg.aimYawLimit,
                                    cfg.aimYawLimit);
    CHECK(std::abs(sim.machine.targetYaw() - expected) <= 10);
    CHECK(count_events(sim.all, RadarEventType::Blip) == 1);

    // Frames from before the head settled do not count
    sim.run(100);
    Contact early = face_contact(sim.now - 60, 1, 0.1f);
    sim.tick(&early);
    CHECK(sim.machine.phase() == Phase::Checking);

    // Two still frames with the face: lock on, aimed from the still frame
    sim.run(cfg.settleMs + 100);
    const int checked_yaw = sim.machine.targetYaw();
    sim.see(0.2f);
    CHECK(sim.machine.phase() == Phase::Checking);
    sim.run(100);
    sim.see(0.2f);
    CHECK(sim.machine.phase() == Phase::Locking);
    CHECK(std::abs(sim.machine.targetYaw() - (checked_yaw + sim.machine.yawOffsetFor(0.2f))) <= 2);
    CHECK(!sim.machine.head().rotate);
}

void test_spin_scan_check_times_out()
{
    auto cfg = spin_config();
    SpinSim sim(cfg);
    sim.start(0);
    sim.run(300);
    Contact blurred = face_contact(sim.now - 120, 1, -0.3f);
    sim.tick(&blurred);
    CHECK(sim.machine.phase() == Phase::Checking);
    // A still frame with it, then one without: starts over, so it never gets two in a row
    sim.run(cfg.settleMs + 750);
    sim.see(0.0f);
    sim.run(100);
    sim.none();
    sim.run(100);
    sim.see(0.0f);
    CHECK(sim.machine.phase() == Phase::Checking);
    // Then nothing: back to spinning
    sim.run(cfg.checkMs + 200);
    CHECK(sim.machine.phase() == Phase::Scanning);
    sim.tick();
    CHECK(sim.machine.head().rotate);
}

void test_spin_scan_cancel_check()
{
    auto cfg = spin_config();
    SpinSim sim(cfg);
    sim.start(0);
    CHECK(face_at_bearing(sim, 600));
    CHECK(sim.machine.phase() == Phase::Checking);
    // The app could not get the head off the dead band: given up, spinning on, that bearing muted
    sim.machine.cancelCheck(sim.now);
    CHECK(sim.machine.phase() == Phase::Scanning);
    sim.tick();
    CHECK(sim.machine.head().rotate);
    CHECK(sim.machine.head().velocity != 0);
    CHECK(face_at_bearing(sim, 650));
    CHECK(sim.machine.phase() == Phase::Scanning);
    sim.machine.cancelCheck(sim.now);  // not checking: nothing happens
    CHECK(sim.machine.phase() == Phase::Scanning);
}

void test_spin_scan_check_waits_for_the_head()
{
    auto cfg = spin_config();
    SpinSim sim(cfg);
    sim.start(0);
    sim.run(300);
    Contact blurred = face_contact(sim.now - 120, 1, 0.2f);
    sim.tick(&blurred);
    CHECK(sim.machine.phase() == Phase::Checking);
    // The app still leaves the sensor's dead band in wheel mode, for longer than the whole check would take: still
    // checking, and faces seen meanwhile do not count
    sim.escaping = true;
    for (uint32_t t = 0; t < cfg.settleMs + cfg.checkMs + 1500; t += 100) {
        sim.see(0.2f, 10);
        sim.run(90);
    }
    CHECK(sim.machine.phase() == Phase::Checking);
    sim.escaping = false;
    // Back in position mode: the turn, the settle time, then two still frames lock on
    sim.run(cfg.settleMs + 100);
    sim.see(0.2f);
    CHECK(sim.machine.phase() == Phase::Checking);
    sim.run(100);
    sim.see(0.2f);
    CHECK(sim.machine.phase() == Phase::Locking);
}

void test_spin_scan_friend_and_limits()
{
    auto cfg = spin_config();
    SpinSim sim(cfg);
    sim.start(0);
    sim.run(300);
    // Lock on, pet: friendly
    Contact blurred = face_contact(sim.now - 120, 1, 0.0f);
    sim.tick(&blurred);
    sim.run(cfg.settleMs + 750);
    sim.see();
    sim.run(100);
    sim.see();
    CHECK(sim.machine.phase() == Phase::Locking);
    RadarInput pet;
    pet.nowMs  = sim.now + 10;
    pet.petted = true;
    sim.now += 10;
    sim.machine.update(pet);
    CHECK(sim.machine.phase() == Phase::Forgiven);
    sim.run(cfg.forgivenMs + cfg.cooldownMs + 100);
    CHECK(sim.machine.phase() == Phase::Scanning);
    // A face while spinning in the grace: a friendly blip, no braking
    sim.all.clear();
    sim.run(200);
    Contact again = face_contact(sim.now - 100, 1, 0.0f);
    sim.tick(&again);
    CHECK(sim.machine.phase() == Phase::Scanning);
    bool friendly = false;
    for (const auto& e : sim.all) {
        friendly = friendly || (e.type == RadarEventType::Blip && e.friendly);
    }
    CHECK(friendly);

    // Right behind the robot (out of the head's reach): a blip, no braking
    SpinSim behind(cfg);
    behind.start(0);
    CHECK(face_at_bearing(behind, 1700));
    CHECK(behind.machine.phase() == Phase::Scanning);
    bool unreachable_blip = false;
    for (const auto& e : behind.all) {
        unreachable_blip = unreachable_blip || (e.type == RadarEventType::Blip && e.unreachable);
    }
    CHECK(unreachable_blip);
    CHECK(!behind.machine.reachable(1700));
    CHECK(!behind.machine.reachable(-1800));
    CHECK(behind.machine.reachable(1450));
    // At the edge of the reach (the face will be near the image edge with the head at its limit): checked
    SpinSim edge(cfg);
    edge.start(0);
    CHECK(face_at_bearing(edge, 1350));
    CHECK(edge.machine.phase() == Phase::Checking);
    CHECK(edge.machine.targetYaw() == cfg.aimYawLimit);
}

void test_spin_scan_mutes_a_failed_check()
{
    auto cfg = spin_config();
    SpinSim sim(cfg);
    sim.start(0);
    // A face at bearing 600 while spinning: checking, nothing confirmed, spin on
    CHECK(face_at_bearing(sim, 600));
    CHECK(sim.machine.phase() == Phase::Checking);
    sim.run(4000);
    CHECK(sim.machine.phase() == Phase::Scanning);
    // From about the same bearing again (10 degree off): no braking for checkMuteMs
    CHECK(face_at_bearing(sim, 700));
    CHECK(sim.machine.phase() == Phase::Scanning);
    // Elsewhere: braking as usual
    CHECK(face_at_bearing(sim, -900));
    CHECK(sim.machine.phase() == Phase::Checking);
    sim.run(4000);
    // After the mute: the first bearing is checked again
    sim.run(cfg.checkMuteMs);
    CHECK(sim.machine.phase() == Phase::Scanning);
    CHECK(face_at_bearing(sim, 600));
    CHECK(sim.machine.phase() == Phase::Checking);
}

void test_motion_fallback_dwell()
{
    Sim sim;
    sim.machine.setDwellScan(400, 1000);
    sim.start(-1000);
    bool settled_seen = false;
    for (int i = 0; i < 600; i++) {
        sim.tick();
        if (sim.machine.dwellSettled(sim.now)) {
            settled_seen = true;
            break;
        }
    }
    CHECK(settled_seen);
    // During the dwell the head command stays put
    const int yaw = sim.machine.head().yaw;
    sim.run(200);
    CHECK(sim.machine.head().yaw == yaw);
    // Motion seen twice in the dwell locks on like a face
    sim.see(200, -0.2f, 0.1f, 0.1f);
    CHECK(sim.machine.phase() == Phase::Locking);
    CHECK(!sim.machine.dwellSettled(sim.now));
}

}  // namespace

int main()
{
    RUN_TEST(test_planner_cruise_and_limits);
    RUN_TEST(test_planner_constant_speed_in_the_middle);
    RUN_TEST(test_planner_starts_inward_at_a_limit);
    RUN_TEST(test_planner_stall_is_bounded);
    RUN_TEST(test_planner_dwell);
    RUN_TEST(test_geometry);
    RUN_TEST(test_scanning_and_pings);
    RUN_TEST(test_lock_on_needs_two_hits);
    RUN_TEST(test_full_sequence);
    RUN_TEST(test_glare_minimum);
    RUN_TEST(test_comes_back_during_cooldown);
    RUN_TEST(test_pet_forgives);
    RUN_TEST(test_friend_grace_renews_while_seen);
    RUN_TEST(test_fine_tracking);
    RUN_TEST(test_aim_is_clamped);
    RUN_TEST(test_yaw_history_follows_the_sweep);
    RUN_TEST(test_motion_fallback_dwell);
    RUN_TEST(test_spin_tracker);
    RUN_TEST(test_spin_scan_closed_loop);
    RUN_TEST(test_spin_scan_closed_loop_real_ticks);
    RUN_TEST(test_spin_stall_watch);
    RUN_TEST(test_spin_scan_fast_turns_then_look_around);
    RUN_TEST(test_spin_scan_look_around_finds_a_face);
    RUN_TEST(test_spin_scan_heat_pause);
    RUN_TEST(test_spin_scan_brakes_turns_back_and_locks);
    RUN_TEST(test_spin_scan_check_times_out);
    RUN_TEST(test_spin_scan_check_waits_for_the_head);
    RUN_TEST(test_spin_scan_cancel_check);
    RUN_TEST(test_spin_scan_friend_and_limits);
    RUN_TEST(test_spin_scan_mutes_a_failed_check);
    std::cout << "all radar logic tests passed (" << stackpet_test::checks() << " checks)\n";
    return 0;
}
