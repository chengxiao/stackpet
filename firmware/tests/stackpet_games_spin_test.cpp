/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "stackpet_games_check.h"
#include <stackpet/games/spin_game.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

using namespace stackpet::games;

namespace {

struct Sim {
    SpinGame game;
    uint32_t now      = 5000;
    bool servo_moving = false;
    std::vector<SpinEvent> events;

    explicit Sim(SpinConfig config = {}) : game(config) {}

    void tick(uint32_t step = 20)
    {
        now += step;
        game.update(now, servo_moving);
        auto fresh = game.takeEvents();
        events.insert(events.end(), fresh.begin(), fresh.end());
    }

    bool runUntil(SpinPhase phase, uint32_t timeoutMs = 20000)
    {
        uint32_t end = now + timeoutMs;
        while (game.phase() != phase && static_cast<int32_t>(end - now) > 0) {
            tick();
        }
        return game.phase() == phase;
    }

    int count(SpinEventType type) const
    {
        int n = 0;
        for (const auto& e : events) {
            n += e.type == type ? 1 : 0;
        }
        return n;
    }
};

void testCategories()
{
    const auto& cats = spinCategories();
    CHECK(cats.size() == 5);
    CHECK(std::strcmp(cats[0].name, "选人") == 0);
    CHECK(cats[0].items.empty());
    CHECK(cats[0].pointAtPerson);
    const char* names[] = {"选人", "吃什么", "真心话", "大冒险", "做决定"};
    for (size_t i = 0; i < cats.size(); i++) {
        CHECK(std::strcmp(cats[i].name, names[i]) == 0);
        if (i > 0) {
            CHECK(cats[i].items.size() >= 12);
        }
        std::set<std::string> unique;
        for (const char* item : cats[i].items) {
            CHECK(item != nullptr);
            CHECK(std::strlen(item) > 0);
            unique.insert(item);
        }
        CHECK(unique.size() == cats[i].items.size());  // no duplicates
    }
    CHECK(!cats[1].pointAtPerson);  // 吃什么: the group decides, face the front
    CHECK(cats[2].pointAtPerson);   // 真心话 picks a victim
    CHECK(cats[3].pointAtPerson);
    CHECK(!cats[4].pointAtPerson);
}

void testVelocityProfile()
{
    SpinConfig config;
    config.seed = 3;
    Sim sim(config);
    CHECK(sim.game.velocity() == 0);
    sim.game.start(sim.now);
    CHECK(sim.game.phase() == SpinPhase::SpinUp);

    int direction = 0;
    int previous  = 0;
    while (sim.game.phase() == SpinPhase::SpinUp) {
        sim.tick();
        int v = sim.game.velocity();
        if (sim.game.phase() != SpinPhase::SpinUp) {
            break;
        }
        CHECK(v != 0);
        if (direction == 0) {
            direction = v > 0 ? 1 : -1;
        }
        CHECK(v * direction > 0);  // never flips direction
        CHECK(std::abs(v) >= std::abs(previous));
        CHECK(std::abs(v) <= config.maxVelocity);
        previous = v;
    }

    CHECK(sim.game.phase() == SpinPhase::Cruise);
    sim.tick();
    CHECK(std::abs(sim.game.velocity()) == config.maxVelocity);
    CHECK(sim.game.velocity() * direction > 0);

    CHECK(sim.runUntil(SpinPhase::SlowDown));
    previous = config.maxVelocity;
    while (sim.game.phase() == SpinPhase::SlowDown) {
        sim.tick();
        if (sim.game.phase() != SpinPhase::SlowDown) {
            break;
        }
        int speed = std::abs(sim.game.velocity());
        CHECK(speed <= previous);
        CHECK(speed >= config.minVelocity);
        CHECK(sim.game.velocity() * direction > 0);
        previous = speed;
    }
    CHECK(sim.game.phase() == SpinPhase::Settle);
    CHECK(sim.game.velocity() == 0);
}

void testTicksFollowTheWheel()
{
    Sim sim;
    sim.game.start(sim.now);
    float last_position = sim.game.wheelPosition();
    while (sim.game.phase() != SpinPhase::Settle) {
        sim.tick();
        CHECK(sim.game.wheelPosition() >= last_position);
        last_position = sim.game.wheelPosition();
    }
    const SpinConfig defaults;
    int ticks = sim.count(SpinEventType::Tick);
    // With at most 4 ticks per 20 ms update nothing is dropped at these speeds
    CHECK(ticks == static_cast<int>(last_position * defaults.ticksPerTurn));
    CHECK(ticks > defaults.ticksPerTurn);  // more than one virtual turn

    // No ticks once the wheel has stopped
    int before = sim.count(SpinEventType::Tick);
    sim.servo_moving = true;
    for (int i = 0; i < 10; i++) {
        sim.tick();
    }
    CHECK(sim.count(SpinEventType::Tick) == before);
}

void testSettleAndReveal()
{
    SpinConfig config;
    Sim sim(config);
    sim.game.setCategory(1);  // 吃什么
    sim.game.start(sim.now);
    CHECK(sim.runUntil(SpinPhase::Settle));
    CHECK(sim.game.resultIndex() == -1);  // hidden until reveal
    CHECK(sim.game.targetYaw() == 0);     // non-pointing category comes back to the front

    // Servo still moving: stays in Settle until the timeout
    sim.servo_moving = true;
    for (uint32_t t = 0; t < config.settleTimeoutMs - 100; t += 20) {
        sim.tick();
    }
    CHECK(sim.game.phase() == SpinPhase::Settle);
    CHECK(sim.runUntil(SpinPhase::Reveal, 300));

    int index = sim.game.resultIndex();
    CHECK(index >= 0);
    CHECK(index < static_cast<int>(sim.game.currentCategory().items.size()));
    CHECK(sim.count(SpinEventType::Reveal) == 1);
    CHECK(sim.events.back().type == SpinEventType::Reveal);
    CHECK(sim.events.back().resultIndex == index);

    // Auto back to Idle after revealMs
    sim.servo_moving = false;
    CHECK(sim.runUntil(SpinPhase::Idle, config.revealMs + 100));
    CHECK(sim.game.resultIndex() == -1);
}

void testPickPersonAndYawLimits()
{
    SpinConfig config;
    config.seed     = 11;
    config.yawLimit = 1100;
    std::set<int> signs;
    for (int round = 0; round < 40; round++) {
        Sim sim(config);
        sim.game.setCategory(0);
        // vary the seed by consuming different amounts of randomness
        for (int k = 0; k < round; k++) {
            sim.game.start(sim.now);
            sim.game.reset();
        }
        sim.game.start(sim.now);
        CHECK(sim.runUntil(SpinPhase::Settle));
        int yaw = sim.game.targetYaw();
        CHECK(yaw >= -config.yawLimit && yaw <= config.yawLimit);
        signs.insert(yaw >= 0 ? 1 : -1);
        CHECK(sim.runUntil(SpinPhase::Reveal));
        CHECK(sim.game.resultIndex() == -1);  // pick-a-person has no item
    }
    CHECK(signs.size() == 2);
}

void testResultsCoverItems()
{
    SpinConfig config;
    config.seed = 1234;
    Sim sim(config);
    sim.game.setCategory(2);  // 真心话
    std::set<int> seen;
    for (int i = 0; i < 120; i++) {
        sim.game.start(sim.now);
        CHECK(sim.runUntil(SpinPhase::Reveal));
        seen.insert(sim.game.resultIndex());
    }
    CHECK(seen.size() > sim.game.currentCategory().items.size() / 2);
}

void testCategorySwitching()
{
    Sim sim;
    CHECK(sim.game.category() == 0);
    sim.game.prevCategory();
    CHECK(sim.game.category() == 4);  // wraps
    sim.game.nextCategory();
    CHECK(sim.game.category() == 0);
    sim.game.setCategory(7);
    CHECK(sim.game.category() == 2);
    sim.game.setCategory(-1);
    CHECK(sim.game.category() == 4);

    // Ignored while spinning, start() ignored too
    sim.game.start(sim.now);
    sim.tick();
    sim.game.nextCategory();
    CHECK(sim.game.category() == 4);
    auto phase_before = sim.game.phase();
    sim.game.start(sim.now);
    CHECK(sim.game.phase() == phase_before);

    // Allowed again from Reveal, and a new spin can start from Reveal
    CHECK(sim.runUntil(SpinPhase::Reveal));
    sim.game.nextCategory();
    CHECK(sim.game.category() == 0);
    sim.game.start(sim.now);
    CHECK(sim.game.phase() == SpinPhase::SpinUp);

    sim.game.reset();
    CHECK(sim.game.phase() == SpinPhase::Idle);
    CHECK(sim.game.velocity() == 0);
    CHECK(sim.game.takeEvents().empty());
}

void testShuffleBagNoRepeats()
{
    SpinConfig config;
    config.seed = 77;
    Sim sim(config);
    sim.game.setCategory(1);  // 吃什么
    const size_t count = sim.game.currentCategory().items.size();
    std::vector<int> results;
    for (size_t i = 0; i < count * 6; i++) {
        sim.game.start(sim.now);
        CHECK(sim.runUntil(SpinPhase::Reveal));
        results.push_back(sim.game.resultIndex());
    }
    for (size_t i = 1; i < results.size(); i++) {
        CHECK(results[i] != results[i - 1]);  // never the same thing twice in a row
    }
    // Every bag cycle shows every item exactly once
    for (size_t start = 0; start + count <= results.size(); start += count) {
        std::set<int> cycle(results.begin() + start, results.begin() + start + count);
        CHECK(cycle.size() == count);
    }
}

void testPickSectorsNeverRepeat()
{
    SpinConfig config;
    config.seed = 5;
    CHECK(config.yawLimit >= 1200 && config.yawLimit <= 1280);
    Sim sim(config);
    sim.game.setCategory(0);  // 选人
    const float width = 2.0f * config.yawLimit / config.pickSectors;
    std::vector<int> yaws;
    for (int i = 0; i < config.pickSectors * 8; i++) {
        sim.game.start(sim.now);
        CHECK(sim.runUntil(SpinPhase::Settle));
        int yaw = sim.game.targetYaw();
        CHECK(yaw >= -config.yawLimit && yaw <= config.yawLimit);
        yaws.push_back(yaw);
        CHECK(sim.runUntil(SpinPhase::Reveal));
    }
    std::set<int> sectors;
    for (size_t i = 0; i < yaws.size(); i++) {
        int sector = std::min(config.pickSectors - 1, static_cast<int>((yaws[i] + config.yawLimit) / width));
        sectors.insert(sector);
        if (i > 0) {
            // Clearly a different direction than last time (neighbour sectors with the max jitter)
            CHECK(std::abs(yaws[i] - yaws[i - 1]) >= static_cast<int>(width * (1.0f - 2.0f * config.sectorJitter)) - 2);
        }
    }
    CHECK(static_cast<int>(sectors.size()) == config.pickSectors);  // people behind the robot get picked too
    CHECK(*std::min_element(yaws.begin(), yaws.end()) < -800);
    CHECK(*std::max_element(yaws.begin(), yaws.end()) > 800);
}

void testRevealStaysWhenRevealMsIsZero()
{
    SpinConfig config;
    config.revealMs = 0;
    Sim sim(config);
    sim.game.setCategory(4);
    sim.game.start(sim.now);
    CHECK(sim.runUntil(SpinPhase::Reveal));
    int result = sim.game.resultIndex();
    for (int i = 0; i < 3000; i++) {  // one minute
        sim.tick();
    }
    CHECK(sim.game.phase() == SpinPhase::Reveal);
    CHECK(sim.game.resultIndex() == result);
    sim.game.start(sim.now);
    CHECK(sim.game.phase() == SpinPhase::SpinUp);
}

}  // namespace

int main()
{
    RUN_TEST(testCategories);
    RUN_TEST(testVelocityProfile);
    RUN_TEST(testTicksFollowTheWheel);
    RUN_TEST(testSettleAndReveal);
    RUN_TEST(testPickPersonAndYawLimits);
    RUN_TEST(testResultsCoverItems);
    RUN_TEST(testCategorySwitching);
    RUN_TEST(testShuffleBagNoRepeats);
    RUN_TEST(testPickSectorsNeverRepeat);
    RUN_TEST(testRevealStaysWhenRevealMsIsZero);
    std::cout << stackpet_test::checks() << " checks passed\n";
    return 0;
}
