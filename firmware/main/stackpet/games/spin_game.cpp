/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "spin_game.h"
#include <algorithm>
#include <cmath>

namespace stackpet::games {

const std::vector<SpinCategory>& spinCategories()
{
    static const std::vector<SpinCategory> categories = {
        {"选人", {}, true, SpinTone::Pick, {}},
        {"吃什么",
         {"火锅", "烧烤", "麻辣烫", "螺蛳粉", "黄焖鸡米饭", "兰州拉面", "饺子", "煎饼果子", "炸鸡配可乐", "披萨", "寿司",
          "汉堡薯条", "酸菜鱼", "小龙虾", "肠粉", "过桥米线", "轻食沙拉", "家常菜", "麻辣香锅", "盖浇饭", "烤肉"},
         false,
         SpinTone::Food,
         {}},
        {"真心话",
         {"说一件最近让你开心的事", "手机里最后一张照片是什么？", "做过最尴尬的事是什么？", "最想去哪里旅行？",
          "小时候的梦想是什么？", "最怕什么小动物？", "最近一次笑到停不下来是因为什么？", "偷偷喜欢过哪个明星？",
          "夸一夸在场的一个人", "最想重来的是哪一天？", "最近一次说谎是什么时候？", "中了五百万先买什么？",
          "最拿手的一道菜是什么？", "相信一见钟情吗？"},
         true,
         SpinTone::Truth,
         {}},
        {"大冒险",
         {"学猫叫三声", "原地转三圈再说“我真棒”", "模仿一个你喜欢的明星", "用头在空中写出你的名字",
          "给在场每个人一句夸奖", "唱两句儿歌", "做十个深蹲", "用方言说一句“我爱你”",
          "大声喊一句“今天真开心！”", "摆一个最帅的姿势拍照", "模仿 StackChan 说话十秒", "和左边的人击个掌",
          "讲一个冷笑话", "闭眼单脚站十秒"},
         true,
         SpinTone::Dare,
         {}},
        // Roughly yes 6 : no 5 : joke 3
        {"做决定",
         {"冲就完了！", "现在就去", "当然可以", "必须的！", "大胆一点", "放心去做吧", "再等等看", "算了吧", "明天再说",
          "绝对不行", "稳一点更好", "问问你的心", "听朋友的", "先吃饭再说"},
         false,
         SpinTone::Yes,
         {SpinTone::Yes, SpinTone::Yes, SpinTone::Yes, SpinTone::Yes, SpinTone::Yes, SpinTone::Yes, SpinTone::No,
          SpinTone::No, SpinTone::No, SpinTone::No, SpinTone::No, SpinTone::Joke, SpinTone::Joke, SpinTone::Joke}},
    };
    return categories;
}

SpinTone spinResultTone(int category, int resultIndex)
{
    const auto& categories = spinCategories();
    if (category < 0 || category >= static_cast<int>(categories.size())) {
        return SpinTone::Pick;
    }
    const auto& cat = categories[category];
    if (resultIndex >= 0 && resultIndex < static_cast<int>(cat.itemTones.size())) {
        return cat.itemTones[resultIndex];
    }
    return cat.tone;
}

SpinGame::SpinGame(const SpinConfig& config) : _config(config)
{
    _rng_state = _config.seed != 0 ? _config.seed : 1;
    _item_bags.resize(spinCategories().size());
    _last_items.assign(spinCategories().size(), -1);
}

static bool is_spinning(SpinPhase phase)
{
    return phase == SpinPhase::SpinUp || phase == SpinPhase::Cruise || phase == SpinPhase::SlowDown ||
           phase == SpinPhase::Settle;
}

void SpinGame::start(uint32_t nowMs)
{
    if (is_spinning(_phase)) {
        return;
    }

    _direction      = (next_random() & 1) ? 1 : -1;
    uint32_t range  = _config.cruiseMaxMs > _config.cruiseMinMs ? _config.cruiseMaxMs - _config.cruiseMinMs : 0;
    _cruise_ms      = _config.cruiseMinMs + (range > 0 ? next_random() % (range + 1) : 0);
    _velocity       = 0;
    _target_yaw     = 0;
    _result_index   = -1;
    _last_tick      = static_cast<int>(std::floor(_wheel_position * _config.ticksPerTurn));
    _last_update_ms = nowMs;
    enter(SpinPhase::SpinUp, nowMs);
}

void SpinGame::reset()
{
    _phase        = SpinPhase::Idle;
    _velocity     = 0;
    _result_index = -1;
    _events.clear();
}

void SpinGame::update(uint32_t nowMs, bool servoMoving)
{
    // Clamp the step so a stalled main loop does not fast-forward the wheel
    uint32_t dt_ms  = std::min<uint32_t>(nowMs - _last_update_ms, 200);
    _last_update_ms = nowMs;

    const uint32_t elapsed = nowMs - _phase_start_ms;
    const int max_v        = std::clamp(std::abs(_config.maxVelocity), 0, 1000);
    const int min_v        = std::clamp(std::abs(_config.minVelocity), 0, max_v);
    int speed              = 0;

    switch (_phase) {
        case SpinPhase::Idle:
            break;

        case SpinPhase::Reveal:
            if (_config.revealMs > 0 && elapsed >= _config.revealMs) {
                enter(SpinPhase::Idle, nowMs);
            }
            break;

        case SpinPhase::SpinUp: {
            float t = _config.spinUpMs > 0 ? std::min(1.0f, static_cast<float>(elapsed) / _config.spinUpMs) : 1.0f;
            speed   = static_cast<int>(min_v + (max_v - min_v) * t);
            if (elapsed >= _config.spinUpMs) {
                enter(SpinPhase::Cruise, nowMs);
            }
            break;
        }

        case SpinPhase::Cruise:
            speed = max_v;
            if (elapsed >= _cruise_ms) {
                enter(SpinPhase::SlowDown, nowMs);
            }
            break;

        case SpinPhase::SlowDown: {
            float t = _config.slowDownMs > 0 ? std::min(1.0f, static_cast<float>(elapsed) / _config.slowDownMs) : 1.0f;
            float remain = (1.0f - t) * (1.0f - t);  // ease-out
            speed        = static_cast<int>(min_v + (max_v - min_v) * remain);
            if (elapsed >= _config.slowDownMs) {
                speed = 0;
                const auto& cat = currentCategory();
                _result_index   = cat.items.empty() ? -1 : draw_item(_category);
                _target_yaw     = cat.pointAtPerson ? draw_target_yaw() : 0;
                enter(SpinPhase::Settle, nowMs);
            }
            break;
        }

        case SpinPhase::Settle:
            if ((elapsed >= _config.settleMinMs && !servoMoving) || elapsed >= _config.settleTimeoutMs) {
                enter(SpinPhase::Reveal, nowMs);
                SpinEvent event;
                event.type        = SpinEventType::Reveal;
                event.phase       = SpinPhase::Reveal;
                event.resultIndex = _result_index;
                _events.push_back(event);
            }
            break;
    }

    // Re-read the phase: a transition above may already have stopped the wheel
    if (_phase == SpinPhase::SpinUp || _phase == SpinPhase::Cruise || _phase == SpinPhase::SlowDown) {
        _velocity = _direction * speed;
    } else {
        _velocity = 0;
    }

    if (_velocity != 0 && max_v > 0) {
        float turns = _config.wheelTurnsPerSec * (static_cast<float>(std::abs(_velocity)) / max_v) * dt_ms / 1000.0f;
        _wheel_position += turns;

        int tick    = static_cast<int>(std::floor(_wheel_position * _config.ticksPerTurn));
        int emitted = 0;
        while (_last_tick < tick && emitted < 4) {
            _last_tick++;
            emitted++;
            SpinEvent event;
            event.type  = SpinEventType::Tick;
            event.phase = _phase;
            _events.push_back(event);
        }
        _last_tick = tick;
    }
}

SpinPhase SpinGame::phase() const
{
    return _phase;
}

int SpinGame::velocity() const
{
    return _velocity;
}

int SpinGame::targetYaw() const
{
    return _target_yaw;
}

int SpinGame::resultIndex() const
{
    return _phase == SpinPhase::Reveal ? _result_index : -1;
}

float SpinGame::wheelPosition() const
{
    return _wheel_position;
}

int SpinGame::category() const
{
    return _category;
}

void SpinGame::setCategory(int index)
{
    if (is_spinning(_phase)) {
        return;
    }
    const int count = static_cast<int>(spinCategories().size());
    _category       = ((index % count) + count) % count;
}

void SpinGame::nextCategory()
{
    setCategory(_category + 1);
}

void SpinGame::prevCategory()
{
    setCategory(_category - 1);
}

const SpinCategory& SpinGame::currentCategory() const
{
    return spinCategories()[_category];
}

std::vector<SpinEvent> SpinGame::takeEvents()
{
    std::vector<SpinEvent> events;
    events.swap(_events);
    return events;
}

void SpinGame::enter(SpinPhase phase, uint32_t nowMs)
{
    _phase          = phase;
    _phase_start_ms = nowMs;

    SpinEvent event;
    event.type        = SpinEventType::PhaseEntered;
    event.phase       = phase;
    event.resultIndex = _result_index;
    _events.push_back(event);
}

void SpinGame::shuffle(std::vector<int>& values)
{
    // Fisher-Yates
    for (size_t i = values.size(); i > 1; i--) {
        size_t j = next_random() % i;
        std::swap(values[i - 1], values[j]);
    }
}

int SpinGame::draw_item(int category)
{
    const int count = static_cast<int>(spinCategories()[category].items.size());
    auto& bag       = _item_bags[category];
    if (bag.empty()) {
        for (int i = 0; i < count; i++) {
            bag.push_back(i);
        }
        shuffle(bag);
        // A fresh bag must not start with the item that ended the previous one
        if (count > 1 && bag.back() == _last_items[category]) {
            std::swap(bag.back(), bag.front());
        }
    }
    int item = bag.back();
    bag.pop_back();
    _last_items[category] = item;
    return item;
}

int SpinGame::draw_target_yaw()
{
    const int limit   = std::max(0, _config.yawLimit);
    const int sectors = std::max(1, _config.pickSectors);
    if (limit == 0) {
        return 0;
    }

    if (_sector_bag.empty()) {
        for (int i = 0; i < sectors; i++) {
            _sector_bag.push_back(i);
        }
        shuffle(_sector_bag);
        if (sectors > 1 && _sector_bag.back() == _last_sector) {
            std::swap(_sector_bag.back(), _sector_bag.front());
        }
    }
    const int sector = _sector_bag.back();
    _sector_bag.pop_back();
    _last_sector = sector;

    // Sector centre plus a bounded jitter, so two picks in neighbouring sectors are still clearly apart
    const float width  = 2.0f * limit / sectors;
    const float centre = -limit + width * (sector + 0.5f);
    const float jitter = std::clamp(_config.sectorJitter, 0.0f, 0.5f) * width;
    const float offset = (static_cast<float>(next_random() % 2001) / 1000.0f - 1.0f) * jitter;
    return std::clamp(static_cast<int>(std::lround(centre + offset)), -limit, limit);
}

uint32_t SpinGame::next_random()
{
    // xorshift32
    uint32_t x = _rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    _rng_state = x != 0 ? x : 1;
    return _rng_state;
}

}  // namespace stackpet::games
