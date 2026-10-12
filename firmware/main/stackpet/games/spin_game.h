/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstdint>
#include <vector>

namespace stackpet::games {

/**
 * "幸运转盘" logic. Pure C++, time is injected, host testable.
 *
 * The yaw servo spins continuously (PWM mode) while the virtual wheel decelerates, then the head settles
 * (position mode) on a random direction and a result of the current category is revealed.
 */
enum class SpinPhase : uint8_t {
    Idle = 0,
    SpinUp,    // velocity ramps 0 -> maxVelocity
    Cruise,    // constant maxVelocity for a random duration
    SlowDown,  // ease-out maxVelocity -> minVelocity
    Settle,    // position mode: move head to targetYaw(); ends when the servo stops (or timeout)
    Reveal,    // show the result, auto return to Idle after revealMs (0 = stay)
};

// What kind of result was drawn: drives the robot's reaction (see game_mood.h)
enum class SpinTone : uint8_t {
    Pick,   // 选人: "就是你！"
    Food,   // 吃什么
    Truth,  // 真心话: an embarrassing question
    Dare,   // 大冒险: a (harmless) punishment
    Yes,    // 做决定: go for it
    No,     // 做决定: better not
    Joke,   // 做决定: a joke answer
};

struct SpinCategory {
    const char* name;                // UTF-8, e.g. "选人"
    std::vector<const char*> items;  // UTF-8; empty => pick-a-person mode, the result is the pointed direction
    bool pointAtPerson = true;       // true: the head stops facing a random person; false: it comes back to the front
    SpinTone tone      = SpinTone::Pick;
    std::vector<SpinTone> itemTones;  // optional per-item tone, parallel to items (empty = `tone` for every item)
};

// Built-in categories: 选人 / 吃什么 / 真心话 / 大冒险 / 做决定.
const std::vector<SpinCategory>& spinCategories();

// Tone of a drawn result (resultIndex -1 = pick-a-person); out of range values fall back to the category tone.
SpinTone spinResultTone(int category, int resultIndex);

struct SpinConfig {
    uint32_t seed            = 1;
    uint32_t spinUpMs        = 600;
    uint32_t cruiseMinMs     = 1500;
    uint32_t cruiseMaxMs     = 3000;
    uint32_t slowDownMs      = 1800;
    int maxVelocity          = 900;   // PWM velocity for Servo::rotate(), -1000..1000
    int minVelocity          = 150;
    int yawLimit             = 1250;  // final yaw target range is [-yawLimit, yawLimit] (0.1 degree, servo max 1280)
    int pickSectors          = 5;     // pick-a-person: the range is split in sectors, never the same sector twice
    float sectorJitter       = 0.3f;  // random offset around a sector centre, as a fraction of the sector width
    uint32_t settleMinMs     = 300;
    uint32_t settleTimeoutMs = 2500;
    uint32_t revealMs        = 8000;  // 0 = stay on the result until the next start()
    int ticksPerTurn         = 12;    // Tick events per virtual wheel turn (one per RGB LED)
    float wheelTurnsPerSec   = 1.6f;  // virtual wheel speed at maxVelocity (drives Tick events and the LED chase)
};

enum class SpinEventType : uint8_t {
    PhaseEntered,  // phase
    Tick,          // the virtual wheel passed one of the ticksPerTurn marks (click sound / LED step)
    Reveal,        // resultIndex is final
};

struct SpinEvent {
    SpinEventType type = SpinEventType::PhaseEntered;
    SpinPhase phase    = SpinPhase::Idle;
    int resultIndex    = -1;
};

class SpinGame {
public:
    explicit SpinGame(const SpinConfig& config = {});

    void start(uint32_t nowMs);  // Idle/Reveal -> SpinUp (ignored while spinning)
    void reset();                // -> Idle without events
    void update(uint32_t nowMs, bool servoMoving);

    SpinPhase phase() const;
    int velocity() const;         // PWM velocity to apply during SpinUp/Cruise/SlowDown, sign = direction; else 0
    int targetYaw() const;        // final yaw angle, valid from Settle on (0 when the category does not point)
    int resultIndex() const;      // item index in the current category, -1 for pick-a-person or before Reveal
                                  // (drawn from a shuffled bag per category: no item repeats before all were shown)
    float wheelPosition() const;  // accumulated virtual wheel position in turns (for LED chase)

    int category() const;
    void setCategory(int index);  // wraps around; ignored while spinning
    void nextCategory();
    void prevCategory();
    const SpinCategory& currentCategory() const;

    std::vector<SpinEvent> takeEvents();

private:
    void enter(SpinPhase phase, uint32_t nowMs);
    int draw_item(int category);
    int draw_target_yaw();
    void shuffle(std::vector<int>& values);
    uint32_t next_random();

    SpinConfig _config;
    SpinPhase _phase          = SpinPhase::Idle;
    uint32_t _phase_start_ms  = 0;
    uint32_t _last_update_ms  = 0;
    uint32_t _cruise_ms       = 0;
    int _direction            = 1;
    int _velocity             = 0;
    int _target_yaw           = 0;
    int _result_index         = -1;
    int _category             = 0;
    float _wheel_position     = 0.0f;
    int _last_tick            = 0;
    uint32_t _rng_state       = 1;
    std::vector<std::vector<int>> _item_bags;  // per category, remaining shuffled item indices
    std::vector<int> _last_items;              // per category, last drawn item (-1 = none)
    std::vector<int> _sector_bag;
    int _last_sector = -1;
    std::vector<SpinEvent> _events;
};

}  // namespace stackpet::games
