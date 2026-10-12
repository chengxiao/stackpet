/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "stackpet_games_check.h"
#include <stackpet/games/freeze_game.h>
#include <algorithm>
#include <cstdint>
#include <vector>

using namespace stackpet::games;

namespace {

constexpr int W = 40;
constexpr int H = 30;

std::vector<uint8_t> makeScene()
{
    std::vector<uint8_t> frame(W * H);
    uint32_t x = 99;
    for (auto& v : frame) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        v = static_cast<uint8_t>(60 + (x % 100));
    }
    return frame;
}

// A moving "person": 8x10 cells = 6.7% of the frame, above the round-0 threshold (4%)
std::vector<uint8_t> withBlock(const std::vector<uint8_t>& scene, int x0, int y0)
{
    auto frame = scene;
    for (int y = y0; y < y0 + 10; y++) {
        for (int x = x0; x < x0 + 8; x++) {
            frame[y * W + x] = 5;
        }
    }
    return frame;
}

// Drives the game with a fake 20 ms clock and a fake 15 fps camera
struct Sim {
    FreezeGame game;
    uint32_t now = 1000;
    uint32_t seq = 0;
    uint32_t last_frame_ms = 0;
    std::vector<uint8_t> scene = makeScene();
    std::vector<uint8_t> frame = scene;
    bool servo_moving         = false;
    // Fake SoundPlayer: each chant word keeps the speaker busy like the real clips do
    bool simulate_audio        = true;
    bool audio_stuck           = false;
    uint32_t audio_busy_until  = 0;
    uint32_t word_audio_ms     = 970;   // zh-CN 1/2/3.ogg
    uint32_t melody_audio_ms   = 1020;  // Sfx::MuTouRen
    std::vector<uint32_t> word_times;
    std::vector<uint32_t> audio_end_times;
    std::vector<FreezeEvent> events;

    explicit Sim(FreezeConfig config = {}) : game(config) {}

    bool audioBusy() const
    {
        return audio_stuck || static_cast<int32_t>(audio_busy_until - now) > 0;
    }

    void tick(bool touched = false)
    {
        now += 20;
        if (now - last_frame_ms >= 66) {
            last_frame_ms = now;
            seq++;
        }
        FreezeInput input;
        input.nowMs       = now;
        input.headTouched = touched;
        input.servoMoving = servo_moving;
        input.audioBusy   = audioBusy();
        input.luma        = frame.data();
        input.lumaWidth   = W;
        input.lumaHeight  = H;
        input.lumaSeq     = seq;
        game.update(input);
        auto fresh = game.takeEvents();
        for (const auto& e : fresh) {
            if (e.type == FreezeEventType::ChantWord) {
                word_times.push_back(now);
                if (simulate_audio) {
                    // The app starts the clip right after update(): busy from the next tick on
                    audio_busy_until = now + (e.index < 3 ? word_audio_ms : melody_audio_ms);
                    audio_end_times.push_back(audio_busy_until);
                }
            }
        }
        events.insert(events.end(), fresh.begin(), fresh.end());
    }

    void run(uint32_t ms)
    {
        uint32_t end = now + ms;
        while (static_cast<int32_t>(end - now) > 0) {
            tick();
        }
    }

    // Run until the game enters `phase` (or timeout). Returns true if reached.
    bool runUntil(FreezePhase phase, uint32_t timeoutMs = 20000)
    {
        uint32_t end = now + timeoutMs;
        while (game.phase() != phase && static_cast<int32_t>(end - now) > 0) {
            tick();
        }
        return game.phase() == phase;
    }

    int count(FreezeEventType type) const
    {
        return static_cast<int>(std::count_if(events.begin(), events.end(), [type](const FreezeEvent& e) {
            return e.type == type;
        }));
    }

    std::vector<FreezePhase> phases() const
    {
        std::vector<FreezePhase> list;
        for (const auto& e : events) {
            if (e.type == FreezeEventType::PhaseEntered) {
                list.push_back(e.phase);
            }
        }
        return list;
    }
};

void testFullRoundWithoutMotion()
{
    Sim sim;
    CHECK(sim.game.phase() == FreezePhase::Idle);
    sim.game.start(sim.now);
    CHECK(sim.game.phase() == FreezePhase::Ready);

    CHECK(sim.runUntil(FreezePhase::Survived));
    std::vector<FreezePhase> expected = {FreezePhase::Ready,    FreezePhase::Chanting, FreezePhase::TurningBack,
                                         FreezePhase::Settling, FreezePhase::Watching, FreezePhase::Survived};
    CHECK(sim.phases() == expected);

    // Four chant words, in order, all while chanting
    std::vector<int> words;
    for (const auto& e : sim.events) {
        if (e.type == FreezeEventType::ChantWord) {
            words.push_back(e.index);
            CHECK(e.phase == FreezePhase::Chanting);
        }
    }
    CHECK((words == std::vector<int>{0, 1, 2, 3}));
    CHECK(sim.count(FreezeEventType::RoundSurvived) == 1);
    CHECK(sim.game.round() == 1);
    CHECK(sim.game.bestRound() == 1);

    // Then the next round starts chanting again
    CHECK(sim.runUntil(FreezePhase::Chanting));
    CHECK(sim.game.round() == 1);
}

void testChantGapsStartAfterAudio()
{
    FreezeConfig config;
    config.seed = 42;
    Sim sim(config);
    sim.game.start(sim.now);

    auto check_round = [&](float scale) {
        sim.word_times.clear();
        sim.audio_end_times.clear();
        CHECK(sim.runUntil(FreezePhase::TurningBack));
        CHECK(sim.word_times.size() == 4);
        CHECK(sim.audio_end_times.size() == 4);
        const uint32_t low  = std::max(config.chantGapFloorMs, static_cast<uint32_t>(config.chantGapMinMs * scale));
        const uint32_t high = std::max(low, static_cast<uint32_t>(config.chantGapMaxMs * scale));
        for (size_t i = 1; i < sim.word_times.size(); i++) {
            // Never talks over itself: the next word comes after the previous clip ended plus the gap
            CHECK(sim.word_times[i] > sim.audio_end_times[i - 1]);
            uint32_t gap = sim.word_times[i] - sim.audio_end_times[i - 1];
            CHECK(gap + 20 >= low);
            CHECK(gap <= high + 40);
        }
    };

    CHECK(sim.runUntil(FreezePhase::Chanting));
    check_round(1.0f);

    // Two rounds later the gaps are shorter but still respect the floor and the audio
    for (int r = 0; r < 2; r++) {
        CHECK(sim.runUntil(FreezePhase::Survived));
        CHECK(sim.runUntil(FreezePhase::Chanting));
    }
    CHECK(sim.game.round() == 2);
    check_round(config.chantSpeedup * config.chantSpeedup);
}

void testGapFloor()
{
    FreezeConfig config;
    CHECK(config.chantGapFloorMs >= 350);
    config.chantGapMinMs = 10;
    config.chantGapMaxMs = 20;
    Sim sim(config);
    sim.game.start(sim.now);
    CHECK(sim.runUntil(FreezePhase::TurningBack));
    for (size_t i = 1; i < sim.word_times.size(); i++) {
        CHECK(sim.word_times[i] - sim.audio_end_times[i - 1] + 20 >= config.chantGapFloorMs);
    }
}

void testTurnBackOnLastNoteAndWatchAfterMelody()
{
    FreezeConfig config;
    config.finalWordTurnDelayMs = 500;
    Sim sim(config);
    sim.melody_audio_ms = 3000;  // a long melody: Watching must wait for it
    sim.game.start(sim.now);
    CHECK(sim.runUntil(FreezePhase::TurningBack));
    uint32_t turn_at = sim.now;
    uint32_t delay   = turn_at - sim.word_times.back();
    CHECK(delay + 20 >= config.finalWordTurnDelayMs);
    CHECK(delay <= config.finalWordTurnDelayMs + 20);

    CHECK(sim.runUntil(FreezePhase::Watching));
    CHECK(!sim.audioBusy());
    CHECK(sim.now >= sim.audio_end_times.back());
}

void testFirstWordWaitsForPlayingSound()
{
    FreezeConfig config;
    Sim sim(config);
    sim.game.start(sim.now);
    // Something (e.g. the countdown tick) is still playing when chanting starts
    while (sim.game.phase() != FreezePhase::Chanting) {
        sim.tick();
        if (sim.game.phase() == FreezePhase::Ready) {
            sim.audio_busy_until = sim.now + 800;
        }
    }
    uint32_t busy_end = sim.audio_busy_until;
    CHECK(sim.runUntil(FreezePhase::TurningBack));
    CHECK(sim.word_times.front() >= busy_end + config.chantGapMinMs - 20);
}

void testAudioNeverBusyOrStuck()
{
    // No sound at all (speaker missing): words still come after the grace time
    Sim silent;
    silent.simulate_audio = false;
    silent.game.start(silent.now);
    CHECK(silent.runUntil(FreezePhase::Watching));
    CHECK(silent.word_times.size() == 4);

    // A sound that never ends must not freeze the game either
    Sim stuck;
    stuck.audio_stuck = true;
    stuck.game.start(stuck.now);
    CHECK(stuck.runUntil(FreezePhase::Watching, 40000));
    CHECK(stuck.word_times.size() == 4);
}

void testFirstRoundTouchGrace()
{
    FreezeConfig config;
    Sim sim(config);
    sim.game.start(sim.now);
    CHECK(sim.runUntil(FreezePhase::Chanting));
    sim.run(500);
    sim.tick(true);  // still next to the robot right after the start: ignored
    CHECK(sim.game.phase() == FreezePhase::Chanting);
    sim.run(config.firstChantTouchGraceMs);
    sim.tick(true);
    CHECK(sim.game.phase() == FreezePhase::PlayerWin);

    // From round 2 on an early touch counts
    Sim sim2(config);
    sim2.game.start(sim2.now);
    CHECK(sim2.runUntil(FreezePhase::Survived));
    CHECK(sim2.runUntil(FreezePhase::Chanting));
    sim2.tick(true);
    CHECK(sim2.game.phase() == FreezePhase::PlayerWin);
}

void testMotionGetsCaught()
{
    Sim sim;
    sim.game.start(sim.now);
    CHECK(sim.runUntil(FreezePhase::Watching));
    sim.run(200);  // reference frame captured
    CHECK(sim.game.phase() == FreezePhase::Watching);
    CHECK(sim.game.motionLevel() < 0.01f);

    // Player moves on the right side of the image
    sim.frame = withBlock(sim.scene, 30, 2);
    CHECK(sim.runUntil(FreezePhase::Caught, 1000));
    auto it = std::find_if(sim.events.begin(), sim.events.end(),
                           [](const FreezeEvent& e) { return e.type == FreezeEventType::Caught; });
    CHECK(it != sim.events.end());
    CHECK(it->x > 0.4f);
    CHECK(it->y < -0.2f);
    CHECK(it->round == 0);
    CHECK(sim.count(FreezeEventType::RoundSurvived) == 0);

    // "Caught you" screen, then the party waits: continue without the caught player, or restart
    sim.frame = sim.scene;
    CHECK(sim.runUntil(FreezePhase::CaughtWait, FreezeConfig{}.caughtMs + 100));
}

void testEliminationContinue()
{
    FreezeConfig config;
    Sim sim(config);
    CHECK(!sim.game.continueGame(sim.now));  // Idle: nothing to continue
    sim.game.start(sim.now);
    CHECK(!sim.game.continueGame(sim.now));  // Ready

    // Survive one round, then get caught in round 2
    CHECK(sim.runUntil(FreezePhase::Survived));
    CHECK(sim.runUntil(FreezePhase::Watching));
    sim.run(200);
    sim.frame = withBlock(sim.scene, 4, 4);
    CHECK(sim.runUntil(FreezePhase::Caught, 1000));
    CHECK(!sim.game.continueGame(sim.now));  // not during the "caught you" presentation
    sim.frame = sim.scene;
    CHECK(sim.runUntil(FreezePhase::CaughtWait, config.caughtMs + 100));
    CHECK(sim.game.round() == 1);

    // Continue: the others survived round 2, chanting starts again and the round counter keeps going
    CHECK(sim.game.continueGame(sim.now));
    CHECK(sim.game.phase() == FreezePhase::Chanting);
    CHECK(sim.game.round() == 2);
    CHECK(sim.game.bestRound() >= 2);
    CHECK(sim.runUntil(FreezePhase::Survived));
    CHECK(sim.game.round() == 3);

    // Nobody continues: back to Idle after the timeout
    FreezeConfig quick = config;
    quick.caughtWaitTimeoutMs = 3000;
    Sim sim2(quick);
    sim2.game.start(sim2.now);
    CHECK(sim2.runUntil(FreezePhase::Watching));
    sim2.run(200);
    sim2.frame = withBlock(sim2.scene, 4, 4);
    CHECK(sim2.runUntil(FreezePhase::CaughtWait, quick.caughtMs + 1500));
    sim2.run(quick.caughtWaitTimeoutMs - 100);
    CHECK(sim2.game.phase() == FreezePhase::CaughtWait);
    CHECK(sim2.runUntil(FreezePhase::Idle, 300));

    // Restart from CaughtWait resets the rounds
    Sim sim3(config);
    sim3.game.start(sim3.now);
    CHECK(sim3.runUntil(FreezePhase::Watching));
    sim3.run(200);
    sim3.frame = withBlock(sim3.scene, 4, 4);
    CHECK(sim3.runUntil(FreezePhase::CaughtWait, config.caughtMs + 1500));
    sim3.game.start(sim3.now);
    CHECK(sim3.game.phase() == FreezePhase::Ready);
    CHECK(sim3.game.round() == 0);

    // Classic mode: straight back to Idle
    FreezeConfig classic = config;
    classic.continueAfterCaught = false;
    Sim sim4(classic);
    sim4.game.start(sim4.now);
    CHECK(sim4.runUntil(FreezePhase::Watching));
    sim4.run(200);
    sim4.frame = withBlock(sim4.scene, 4, 4);
    CHECK(sim4.runUntil(FreezePhase::Caught, 1000));
    sim4.frame = sim4.scene;
    CHECK(sim4.runUntil(FreezePhase::Idle, classic.caughtMs + 100));
}

void testSingleNoisyFrameIsNotEnough()
{
    Sim sim;
    sim.game.start(sim.now);
    CHECK(sim.runUntil(FreezePhase::Watching));
    sim.run(200);

    // One frame with motion, then the scene is frozen again: confirmFrames = 2 must reject it
    sim.frame     = withBlock(sim.scene, 10, 10);
    uint32_t seq0 = sim.seq;
    while (sim.seq == seq0) {
        sim.tick();
    }
    sim.frame = sim.scene;
    CHECK(sim.runUntil(FreezePhase::Survived));
    CHECK(sim.count(FreezeEventType::Caught) == 0);
}

void testRepeatedFrameSeqIsAnalysedOnce()
{
    FreezeGame game;
    auto scene  = makeScene();
    auto moving = withBlock(scene, 10, 10);
    uint32_t now = 0;
    game.start(now);

    FreezeInput input;
    input.lumaWidth  = W;
    input.lumaHeight = H;
    input.luma       = scene.data();
    input.lumaSeq    = 1;
    // Fast-forward to Watching without new frames
    for (int i = 0; i < 1000 && game.phase() != FreezePhase::Watching; i++) {
        now += 20;
        input.nowMs = now;
        game.update(input);
    }
    CHECK(game.phase() == FreezePhase::Watching);

    // Reference from a new frame
    now += 20;
    input.nowMs   = now;
    input.lumaSeq = 2;
    game.update(input);

    // The same moving frame delivered many times with the same seq counts once
    input.luma    = moving.data();
    input.lumaSeq = 3;
    for (int i = 0; i < 20; i++) {
        now += 20;
        input.nowMs = now;
        game.update(input);
    }
    CHECK(game.phase() == FreezePhase::Watching);
    CHECK(game.motionLevel() > game.currentThreshold());

    // A second distinct frame with motion confirms it
    now += 20;
    input.nowMs   = now;
    input.lumaSeq = 4;
    game.update(input);
    CHECK(game.phase() == FreezePhase::Caught);
}

void testTouchWins()
{
    // Touch while chanting => win, then Idle again
    Sim sim;
    sim.game.start(sim.now);
    sim.tick(true);  // touch during Ready is ignored
    CHECK(sim.game.phase() == FreezePhase::Ready);
    CHECK(sim.runUntil(FreezePhase::Chanting));
    sim.run(FreezeConfig{}.firstChantTouchGraceMs + 20);
    sim.tick(true);
    CHECK(sim.game.phase() == FreezePhase::PlayerWin);
    // The win screen lasts winMs even if the previous phase was long
    sim.run(FreezeConfig{}.winMs - 100);
    CHECK(sim.game.phase() == FreezePhase::PlayerWin);
    CHECK(sim.runUntil(FreezePhase::Idle, 300));

    // Touch in Idle does nothing
    sim.tick(true);
    CHECK(sim.game.phase() == FreezePhase::Idle);

    // Touch late in a long chanting phase must still show the full win screen
    FreezeConfig slow;
    slow.chantGapMinMs = 3000;
    slow.chantGapMaxMs = 3000;
    Sim sim2(slow);
    sim2.game.start(sim2.now);
    CHECK(sim2.runUntil(FreezePhase::Chanting));
    sim2.run(8000);
    CHECK(sim2.game.phase() == FreezePhase::Chanting);
    sim2.tick(true);
    CHECK(sim2.game.phase() == FreezePhase::PlayerWin);
    sim2.tick();
    CHECK(sim2.game.phase() == FreezePhase::PlayerWin);
}

void testTurnBackWaitsForServo()
{
    Sim sim;
    sim.game.start(sim.now);
    CHECK(sim.runUntil(FreezePhase::TurningBack));
    sim.servo_moving = true;
    sim.run(FreezeConfig{}.turnBackMinMs + 200);
    CHECK(sim.game.phase() == FreezePhase::TurningBack);
    sim.servo_moving = false;
    sim.tick();
    CHECK(sim.game.phase() == FreezePhase::Settling);

    // Timeout if the servo never reports stopped
    Sim sim2;
    sim2.game.start(sim2.now);
    CHECK(sim2.runUntil(FreezePhase::TurningBack));
    sim2.servo_moving = true;
    CHECK(sim2.runUntil(FreezePhase::Settling, FreezeConfig{}.turnBackTimeoutMs + 100));
}

void testDifficultyScaling()
{
    FreezeConfig config;
    Sim sim(config);
    float threshold0 = sim.game.currentThreshold();
    uint32_t watch0  = sim.game.watchDurationMs();
    CHECK_NEAR(threshold0, config.motionThreshold, 1e-6f);
    CHECK(watch0 == config.watchBaseMs);

    sim.game.start(sim.now);
    float previous = threshold0;
    for (int r = 1; r <= 12; r++) {
        CHECK(sim.runUntil(FreezePhase::Survived));
        CHECK(sim.game.round() == r);
        float t = sim.game.currentThreshold();
        CHECK(t <= previous + 1e-6f);
        CHECK(t >= config.thresholdFloor - 1e-6f);
        previous = t;
        CHECK(sim.game.watchDurationMs() <= config.watchMaxMs);
        CHECK(sim.runUntil(FreezePhase::Chanting));
    }
    CHECK_NEAR(sim.game.currentThreshold(), config.thresholdFloor, 1e-6f);
    CHECK(sim.game.watchDurationMs() == config.watchMaxMs);

    // start() resets the round counter but keeps the best result
    sim.game.start(sim.now);
    CHECK(sim.game.round() == 0);
    CHECK(sim.game.bestRound() == 12);
}

void testResetAndMotionLevel()
{
    Sim sim;
    sim.game.start(sim.now);
    CHECK(sim.game.motionLevel() == 0.0f);
    CHECK(sim.runUntil(FreezePhase::Watching));
    sim.run(200);
    sim.frame = withBlock(sim.scene, 0, 0);
    uint32_t seq0 = sim.seq;
    while (sim.seq == seq0) {
        sim.tick();
    }
    CHECK(sim.game.motionLevel() > 0.02f);

    sim.game.reset();
    CHECK(sim.game.phase() == FreezePhase::Idle);
    CHECK(sim.game.takeEvents().empty());
    CHECK(sim.game.motionLevel() == 0.0f);
}

void testContinueGetsTouchGraceAndNoRecord()
{
    FreezeConfig config;
    Sim sim(config);
    sim.game.start(sim.now);
    CHECK(!sim.game.hasContinued());
    CHECK(sim.runUntil(FreezePhase::Watching));
    sim.run(200);
    sim.frame = withBlock(sim.scene, 4, 4);
    CHECK(sim.runUntil(FreezePhase::CaughtWait, config.caughtMs + 1500));
    sim.frame = sim.scene;
    CHECK(!sim.game.hasContinued());

    // The player who walked up to tap "continue" stands next to the robot: an early touch must not win
    CHECK(sim.game.continueGame(sim.now));
    CHECK(sim.game.hasContinued());
    CHECK(sim.game.phase() == FreezePhase::Chanting);
    sim.run(500);
    sim.tick(true);
    CHECK(sim.game.phase() == FreezePhase::Chanting);
    sim.run(config.firstChantTouchGraceMs);
    sim.tick(true);
    CHECK(sim.game.phase() == FreezePhase::PlayerWin);
    CHECK(sim.game.hasContinued());  // still the same game

    // The grace only covers the first chant after continue: the following round has none
    Sim sim2(config);
    sim2.game.start(sim2.now);
    CHECK(sim2.runUntil(FreezePhase::Watching));
    sim2.run(200);
    sim2.frame = withBlock(sim2.scene, 4, 4);
    CHECK(sim2.runUntil(FreezePhase::CaughtWait, config.caughtMs + 1500));
    sim2.frame = sim2.scene;
    CHECK(sim2.game.continueGame(sim2.now));
    CHECK(sim2.runUntil(FreezePhase::Survived));
    CHECK(sim2.runUntil(FreezePhase::Chanting));
    sim2.tick(true);
    CHECK(sim2.game.phase() == FreezePhase::PlayerWin);

    // A new game clears the flag
    sim2.game.start(sim2.now);
    CHECK(!sim2.game.hasContinued());
}

}  // namespace

int main()
{
    RUN_TEST(testFullRoundWithoutMotion);
    RUN_TEST(testChantGapsStartAfterAudio);
    RUN_TEST(testGapFloor);
    RUN_TEST(testTurnBackOnLastNoteAndWatchAfterMelody);
    RUN_TEST(testFirstWordWaitsForPlayingSound);
    RUN_TEST(testAudioNeverBusyOrStuck);
    RUN_TEST(testFirstRoundTouchGrace);
    RUN_TEST(testMotionGetsCaught);
    RUN_TEST(testEliminationContinue);
    RUN_TEST(testContinueGetsTouchGraceAndNoRecord);
    RUN_TEST(testSingleNoisyFrameIsNotEnough);
    RUN_TEST(testRepeatedFrameSeqIsAnalysedOnce);
    RUN_TEST(testTouchWins);
    RUN_TEST(testTurnBackWaitsForServo);
    RUN_TEST(testDifficultyScaling);
    RUN_TEST(testResetAndMotionLevel);
    std::cout << stackpet_test::checks() << " checks passed\n";
    return 0;
}
