/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
// Host tests of the remote's packets, the stick gestures and the launcher's stick (stackpet/remote/*)
#include "stackpet_games_check.h"
#include <stackpet/remote/remote_packet.h>
#include <stackpet/remote/stick_gestures.h>
#include <stackpet/remote/stick_navigator.h>
#include <cmath>
#include <iostream>
#include <vector>

using namespace stackpet::remote;

namespace {

RemotePacket stick(float x, float y, bool laser = false, int buttons = -1)
{
    RemotePacket p;
    p.yaw   = static_cast<int>(std::lround(x * 1280.0f));
    p.pitch = static_cast<int>(std::lround(450.0f + y * 450.0f));
    p.speed = 600;
    p.laser = laser;
    if (buttons >= 0) {
        p.hasButtons = true;
        p.buttons    = static_cast<uint8_t>(buttons);
    }
    return p;
}

std::vector<uint8_t> bytes(uint8_t target, int16_t yaw, int16_t pitch, int16_t speed, uint8_t laser)
{
    return {target,
            static_cast<uint8_t>(yaw & 0xFF),
            static_cast<uint8_t>((yaw >> 8) & 0xFF),
            static_cast<uint8_t>(pitch & 0xFF),
            static_cast<uint8_t>((pitch >> 8) & 0xFF),
            static_cast<uint8_t>(speed & 0xFF),
            static_cast<uint8_t>((speed >> 8) & 0xFF),
            laser};
}

int count(const std::vector<GestureEvent>& events, Gesture g)
{
    int n = 0;
    for (const auto& e : events) {
        n += e.gesture == g ? 1 : 0;
    }
    return n;
}

// Feeds a path of stick positions, `stepMs` apart; returns every gesture seen
std::vector<GestureEvent> feed(StickGestures& g, uint32_t& now, const std::vector<std::pair<float, float>>& path,
                               uint32_t stepMs)
{
    std::vector<GestureEvent> all;
    for (const auto& p : path) {
        now += stepMs;
        g.update(now, stick(p.first, p.second));
        for (const auto& e : g.take()) {
            all.push_back(e);
        }
    }
    return all;
}

// Out to +-level along one axis and back, `flicks` times, through the centre
std::vector<std::pair<float, float>> flicks(bool yawAxis, int flicks, float level = 0.85f)
{
    std::vector<std::pair<float, float>> path;
    for (int i = 0; i < flicks; i++) {
        const float side = (i % 2 == 0) ? 1.0f : -1.0f;
        for (float v : {0.35f, 0.7f, 1.0f, 0.7f, 0.35f, 0.0f}) {
            const float a = v * level * side;
            path.push_back(yawAxis ? std::make_pair(a, 0.05f) : std::make_pair(0.05f, a));
        }
    }
    return path;
}

std::vector<std::pair<float, float>> sweep(float fromDeg, float toDeg, float radius, float stepDeg)
{
    std::vector<std::pair<float, float>> path;
    const float step = toDeg > fromDeg ? stepDeg : -stepDeg;
    for (float a = fromDeg; (step > 0) ? a <= toDeg : a >= toDeg; a += step) {
        const float rad = a / 57.2957795f;
        path.emplace_back(radius * std::cos(rad), radius * std::sin(rad));
    }
    return path;
}

void test_packets()
{
    // The official remote's 8 bytes: yaw -1000, pitch 300, speed 600, laser on, for every robot
    auto b = bytes(0, -1000, 300, 600, 1);
    auto p = parsePacket(b.data(), b.size());
    CHECK(p.has_value());
    CHECK(p->target == 0 && p->yaw == -1000 && p->pitch == 300 && p->speed == 600 && p->laser);
    CHECK(!p->hasButtons && p->channel == 0);
    CHECK(isForReceiver(*p, 1) && isForReceiver(*p, 7));
    // StackPet's remote: two more bytes, the buttons held (the stick pressed in, the M5 button) and the channel it
    // sends on
    b.push_back(kButtonStick);
    p = parsePacket(b.data(), b.size());
    CHECK(p.has_value() && p->hasButtons && p->buttons == kButtonStick && p->channel == 0);
    b.push_back(6);
    p = parsePacket(b.data(), b.size());
    CHECK(p.has_value() && p->hasButtons && p->buttons == kButtonStick && p->channel == 6);
    b[8] = kButtonStick | kButtonHome;
    p    = parsePacket(b.data(), b.size());
    CHECK(p.has_value() && p->buttons == (kButtonStick | kButtonHome));
    // For robot 3 only
    auto own = bytes(3, 0, 450, 600, 0);
    p        = parsePacket(own.data(), own.size());
    CHECK(p.has_value() && isForReceiver(*p, 3) && !isForReceiver(*p, 1));
    // Not from the remote: other sizes, values it never sends, unknown button bits, no such channel
    CHECK(!parsePacket(b.data(), 7).has_value());
    CHECK(parsePacket(b.data(), b.size())->homeCount == -1);  // 10 bytes: no count
    // 11: the M5 button's presses so far; the press's copy on every channel (bit 2)
    auto counted = b;
    counted[8]   = kButtonHome | kButtonEverywhere;
    counted.push_back(200);
    p = parsePacket(counted.data(), counted.size());
    CHECK(p.has_value() && p->homeCount == 200 && p->buttons == (kButtonHome | kButtonEverywhere) && p->channel == 6);
    auto longer = counted;
    longer.push_back(0);
    CHECK(!parsePacket(longer.data(), longer.size()).has_value());
    CHECK(!parsePacket(nullptr, 8).has_value());
    for (uint8_t channel : {0, 15, 0xFF}) {
        auto odd = bytes(0, 0, 450, 600, 0);
        odd.push_back(0);
        odd.push_back(channel);
        CHECK(!parsePacket(odd.data(), odd.size()).has_value());
    }
    // A stick that goes further than the remote's calibration: within the range again
    auto over = bytes(0, 1800, -88, 600, 0);
    p         = parsePacket(over.data(), over.size());
    CHECK(p.has_value() && p->yaw == 1280 && p->pitch == 0);
    over = bytes(0, -1581, 1081, 600, 0);
    p    = parsePacket(over.data(), over.size());
    CHECK(p.has_value() && p->yaw == -1280 && p->pitch == 900);
    auto wild = bytes(0, 5000, 450, 600, 0);
    CHECK(!parsePacket(wild.data(), wild.size()).has_value());
    wild = bytes(0, 0, 450, 600, 7);
    CHECK(!parsePacket(wild.data(), wild.size()).has_value());
    wild = bytes(0, 0, 2000, 600, 0);
    CHECK(!parsePacket(wild.data(), wild.size()).has_value());
    for (uint8_t unknown : {0x08, 0x80}) {
        auto bits = bytes(0, 0, 450, 600, 0);
        bits.push_back(unknown);
        CHECK(!parsePacket(bits.data(), bits.size()).has_value());
    }
    // How far the stick is off the centre
    CHECK(stickDeflection(stick(0.0f, 0.0f)) < 0.01f);
    CHECK(std::fabs(stickDeflection(stick(-0.7f, 0.2f)) - 0.7f) < 0.01f);
    CHECK(std::fabs(stickDeflection(stick(0.1f, 1.0f)) - 1.0f) < 0.01f);
}

void test_shake_and_nod()
{
    StickGestures g;
    uint32_t now = 1000;
    // Left, right, left, right quickly (40 ms between packets): one shake
    auto events = feed(g, now, flicks(true, 4), 40);
    CHECK(count(events, Gesture::Shake) == 1);
    CHECK(count(events, Gesture::Nod) == 0);
    CHECK(count(events, Gesture::Circle) == 0);
    // Right after it, the same again (0.96 s): the stick has to settle first, too few flicks are left after that
    events = feed(g, now, flicks(true, 4), 40);
    CHECK(count(events, Gesture::Shake) == 0);
    now += 2000;
    events = feed(g, now, flicks(true, 4), 40);
    CHECK(count(events, Gesture::Shake) == 1);
    // Up and down: a nod
    now += 2000;
    events = feed(g, now, flicks(false, 4), 40);
    CHECK(count(events, Gesture::Nod) == 1);
    CHECK(count(events, Gesture::Shake) == 0);
    // The same flicks slowly (300 ms between packets, 1.8 s a flick): steering, nothing
    now += 2000;
    events = feed(g, now, flicks(true, 4), 300);
    CHECK(events.empty());
    // Not far enough out: nothing
    now += 2000;
    events = feed(g, now, flicks(true, 6, 0.4f), 40);
    CHECK(events.empty());
}

void test_steering_is_no_gesture()
{
    StickGestures g;
    uint32_t now = 1000;
    // Full left, hold, full right, hold, up, hold, back: what steering looks like
    std::vector<std::pair<float, float>> path;
    for (int i = 0; i < 20; i++) {
        path.emplace_back(-0.05f * i, 0.0f);
    }
    for (int i = 0; i < 25; i++) {
        path.emplace_back(-1.0f, 0.0f);
    }
    for (int i = 0; i < 40; i++) {
        path.emplace_back(-1.0f + 0.05f * i, 0.0f);
    }
    for (int i = 0; i < 25; i++) {
        path.emplace_back(1.0f, 0.1f);
    }
    for (int i = 0; i < 20; i++) {
        path.emplace_back(1.0f - 0.05f * i, 0.05f * i);
    }
    auto events = feed(g, now, path, 40);
    CHECK(events.empty());
}

void test_circle()
{
    StickGestures g;
    uint32_t now = 1000;
    // Round the edge counter-clockwise in about 1.3 s: a circle, not a shake or a nod
    auto events = feed(g, now, sweep(0.0f, 360.0f, 0.9f, 10.0f), 35);
    CHECK(count(events, Gesture::Circle) == 1);
    CHECK(count(events, Gesture::Shake) == 0);
    CHECK(count(events, Gesture::Nod) == 0);
    CHECK(events.size() == 1 && events[0].direction == 1);
    // Clockwise
    now += 2000;
    events = feed(g, now, sweep(90.0f, -270.0f, 0.9f, 10.0f), 35);
    CHECK(events.size() == 1 && events[0].gesture == Gesture::Circle && events[0].direction == -1);
    // Only three quarters of the way: nothing
    now += 2000;
    events = feed(g, now, sweep(0.0f, 270.0f, 0.9f, 10.0f), 35);
    CHECK(events.empty());
    // Through the centre half way round: starts over, nothing
    now += 2000;
    auto half = sweep(0.0f, 180.0f, 0.9f, 10.0f);
    half.emplace_back(0.0f, 0.0f);
    for (const auto& p : sweep(180.0f, 360.0f, 0.9f, 10.0f)) {
        half.push_back(p);
    }
    events = feed(g, now, half, 35);
    CHECK(events.empty());
    // Too slow (10 s a turn): nothing
    now += 2000;
    events = feed(g, now, sweep(0.0f, 360.0f, 0.9f, 10.0f), 280);
    CHECK(events.empty());
    // One and a half turns: one circle (the rest falls in its quiet time), no shake or nod on the way
    now += 2000;
    events = feed(g, now, sweep(0.0f, 540.0f, 0.9f, 10.0f), 35);
    CHECK(events.size() == 1 && events[0].gesture == Gesture::Circle);
}

void test_quiet_time_and_buttons()
{
    // A shake, then within its quiet time B twice: still counts
    StickGestures g;
    uint32_t now = 1000;
    g.update(now, stick(0.0f, 0.0f, false, 0));
    auto events = feed(g, now, flicks(true, 4), 40);
    CHECK(count(events, Gesture::Shake) == 1);
    now += 40;
    g.update(now, stick(0.0f, 0.0f, true, 0));
    now += 200;
    g.update(now, stick(0.0f, 0.0f, false, 0));
    CHECK(count(g.take(), Gesture::DoubleClick) == 1);
}

void test_first_packet()
{
    // The laser on in the very first packet is where it starts: off and on again quickly is one double click
    StickGestures laser;
    laser.update(1000, stick(0.0f, 0.0f, true, 0));
    laser.update(1100, stick(0.0f, 0.0f, false, 0));
    laser.update(1200, stick(0.0f, 0.0f, true, 0));
    const auto events = laser.take();
    CHECK(events.size() == 1 && events[0].gesture == Gesture::DoubleClick);
}

void test_diagonal_is_no_shake()
{
    // Flicked to and fro along a diagonal (corner to corner): neither a shake nor a nod
    StickGestures g;
    uint32_t now = 1000;
    std::vector<std::pair<float, float>> path;
    for (int i = 0; i < 6; i++) {
        const float side = (i % 2 == 0) ? 1.0f : -1.0f;
        for (float v : {0.3f, 0.6f, 0.75f, 0.6f, 0.3f, 0.0f}) {
            path.emplace_back(v * side, v * side);
        }
    }
    const auto events = feed(g, now, path, 40);
    CHECK(count(events, Gesture::Shake) == 0 && count(events, Gesture::Nod) == 0);
}

void test_clock_wrap()
{
    // The millisecond clock wraps in the middle of a shake, and of the quiet time after it
    StickGestures g;
    uint32_t now = 0xFFFFFFFFu - 300;
    auto events  = feed(g, now, flicks(true, 4), 40);
    CHECK(count(events, Gesture::Shake) == 1);
    now += 2000;
    events = feed(g, now, flicks(false, 4), 40);
    CHECK(count(events, Gesture::Nod) == 1);
    // A long quiet spell (30 days) after a gesture: the next one counts
    now += 30u * 24u * 3600u * 1000u;
    events = feed(g, now, flicks(true, 4), 40);
    CHECK(count(events, Gesture::Shake) == 1);
}

void test_buttons()
{
    StickGestures g;
    uint32_t now = 1000;
    g.update(now, stick(0.0f, 0.0f, false, 0));
    // B twice quickly (the laser flag flips twice): a double click
    now += 100;
    g.update(now, stick(0.0f, 0.0f, true, 0));
    now += 300;
    g.update(now, stick(0.0f, 0.0f, false, 0));
    auto events = g.take();
    CHECK(count(events, Gesture::DoubleClick) == 1);
    // Twice, a second apart: nothing
    now += 1000;
    g.update(now, stick(0.0f, 0.0f, true, 0));
    now += 1000;
    g.update(now, stick(0.0f, 0.0f, false, 0));
    CHECK(g.take().empty());
    // The stick pressed in, the M5 button: no gesture (the launcher's, the home screen's)
    now += 100;
    g.update(now, stick(0.0f, 0.0f, false, kButtonStick));
    now += 40;
    g.update(now, stick(0.0f, 0.0f, false, kButtonHome));
    now += 40;
    g.update(now, stick(0.0f, 0.0f, false, 0));
    CHECK(g.take().empty());
    // reset() forgets the laser flag: the first packet after it is where it starts again
    g.reset();
    g.update(now + 40, stick(0.0f, 0.0f, true, 0));
    g.update(now + 80, stick(0.1f, 0.0f, true, 0));
    CHECK(g.take().empty());
}

// The launcher's stick: `towardsNext` along yaw (kNextYaw: which way that is), the buttons held
RemotePacket pushed(float towardsNext, int buttons = 0, float y = 0.0f)
{
    return stick(towardsNext * kNextYaw, y, false, buttons);
}

// The official remote: no buttons byte
RemotePacket official(float towardsNext)
{
    return stick(towardsNext * kNextYaw, 0.0f);
}

int count(const std::vector<NavAction>& actions, NavAction a)
{
    int n = 0;
    for (const auto x : actions) {
        n += x == a ? 1 : 0;
    }
    return n;
}

// Ticks every 30 ms (the main loop) for ms; every action seen
std::vector<NavAction> idle(StickNavigator& nav, uint32_t& now, uint32_t ms)
{
    std::vector<NavAction> all;
    for (uint32_t t = 0; t < ms; t += 30) {
        now += 30;
        nav.tick(now);
        for (const auto a : nav.take()) {
            all.push_back(a);
        }
    }
    return all;
}

void test_nav_steps()
{
    StickNavigator nav;
    nav.reset(0);
    uint32_t now = 5000;
    // Pushed right: the next app, once while it stays out there
    nav.update(now, pushed(0.3f));
    nav.update(now += 40, pushed(0.8f));
    nav.update(now += 40, pushed(1.0f));
    auto got = nav.take();
    CHECK(got.size() == 1 && got[0] == NavAction::Next);
    // Eased off, not back in the middle (0.4): out again is no new step
    nav.update(now += 40, pushed(0.4f));
    nav.update(now += 40, pushed(0.8f));
    CHECK(nav.take().empty());
    // Back in the middle, out again: the next one
    nav.update(now += 40, pushed(0.1f));
    nav.update(now += 40, pushed(0.7f));
    got = nav.take();
    CHECK(got.size() == 1 && got[0] == NavAction::Next);
    // Straight over to the left: the previous one
    nav.update(now += 40, pushed(-0.9f));
    got = nav.take();
    CHECK(got.size() == 1 && got[0] == NavAction::Previous);
    // Not far enough out, or up and down: nothing
    nav.update(now += 40, pushed(0.0f));
    nav.update(now += 40, pushed(0.45f));
    nav.update(now += 40, pushed(0.0f, 0, 1.0f));
    nav.update(now += 40, pushed(0.0f, 0, -1.0f));
    nav.update(now += 40, pushed(0.0f));
    CHECK(nav.take().empty());
    // The official remote (no buttons byte) steps all the same
    StickNavigator plain;
    plain.reset(0);
    plain.update(5000, official(-0.9f));
    got = plain.take();
    CHECK(got.size() == 1 && got[0] == NavAction::Previous);
    // Pressed in by mistake out there (easy at the edge), let go still out there: no second step
    StickNavigator slip;
    slip.reset(0);
    now = 5000;
    slip.update(now, pushed(0.9f));
    slip.update(now += 40, pushed(0.9f, kButtonStick));
    slip.update(now += 40, pushed(0.9f));
    CHECK(count(slip.take(), NavAction::Next) == 1);
    // Eased off to 0.4 (no new step from there), pressed in there, pushed out again: no step either
    slip.update(now += 40, pushed(0.4f));
    slip.update(now += 40, pushed(0.4f, kButtonStick));
    slip.update(now += 40, pushed(0.4f));
    slip.update(now += 40, pushed(0.9f));
    CHECK(slip.take().empty());
    // A long press begun in the middle, pushed out while held (no open), let go out there: no step
    slip.update(now += 40, pushed(0.0f));
    slip.update(now += 40, pushed(0.0f, kButtonStick));
    slip.update(now += 300, pushed(0.9f, kButtonStick));
    slip.update(now += 40, pushed(0.9f));
    uint32_t t = now;
    CHECK(idle(slip, t, 2000).empty());
    // Flicked out fast, pressed by mistake already in the first packet out there, let go out there: that is the push,
    // one step (and only one)
    StickNavigator flick;
    flick.reset(0);
    now = 5000;
    flick.update(now, pushed(0.0f));
    flick.update(now += 40, pushed(0.95f, kButtonStick));
    CHECK(flick.take().empty());
    flick.update(now += 40, pushed(0.95f));
    flick.update(now += 40, pushed(1.0f));
    CHECK(count(flick.take(), NavAction::Next) == 1);
    // ... held pressed out there 0.3 s before letting go: still that push
    flick.update(now += 40, pushed(0.0f));
    flick.update(now += 40, pushed(0.95f, kButtonStick));
    flick.update(now += 300, pushed(0.95f));
    CHECK(count(flick.take(), NavAction::Next) == 1);
    // A long press tried off the middle (too far out to count, no bar), held 1 s, let go still off it: no push either
    StickNavigator slanted;
    slanted.reset(0);
    now = 5000;
    slanted.update(now, pushed(0.0f));
    slanted.update(now += 40, pushed(0.6f, kButtonStick));
    CHECK(slanted.openProgress(now) == 0.0f);
    t = now;
    CHECK(idle(slanted, t, 1000).empty());
    now = t;
    slanted.update(now += 40, pushed(0.6f));
    t = now;
    CHECK(idle(slanted, t, 2000).empty());
}

void test_nav_repeat()
{
    StickNavigator nav;
    nav.reset(0);
    uint32_t now = 5000;
    nav.update(now, pushed(1.0f));
    CHECK(count(nav.take(), NavAction::Next) == 1);
    // Held out there (the remote sends nothing meanwhile): on after 0.6 s, then every 0.45 s
    nav.tick(now + 599);
    CHECK(nav.take().empty());
    nav.tick(now + 600);
    CHECK(count(nav.take(), NavAction::Next) == 1);
    nav.tick(now + 1049);
    CHECK(nav.take().empty());
    nav.tick(now + 1050);
    CHECK(count(nav.take(), NavAction::Next) == 1);
    // Held for good (a lost packet): 10 more at most
    now += 1050;
    const auto more = idle(nav, now, 20000);
    CHECK(count(more, NavAction::Next) == 8 && count(more, NavAction::Previous) == 0);
    // Eased off (0.4): no more going on, also once out there again without coming back
    nav.update(now += 40, pushed(-0.4f));
    nav.update(now += 40, pushed(0.0f));
    nav.update(now += 40, pushed(-1.0f));
    CHECK(count(nav.take(), NavAction::Previous) == 1);
    nav.update(now += 40, pushed(-0.4f));
    CHECK(idle(nav, now, 3000).empty());
    nav.update(now += 40, pushed(-0.9f));
    CHECK(idle(nav, now, 3000).empty());
    // Pressed in by mistake while going on: it stops, also once let go out there
    nav.update(now += 40, pushed(0.0f));
    nav.update(now += 40, pushed(-1.0f));
    CHECK(count(nav.take(), NavAction::Previous) == 1);
    nav.update(now += 40, pushed(-1.0f, kButtonStick));
    nav.update(now += 40, pushed(-1.0f));
    CHECK(idle(nav, now, 3000).empty());
    // The packets alone go on with nothing: only tick() does
    nav.update(now += 40, pushed(0.0f));
    nav.update(now += 40, pushed(1.0f));
    CHECK(count(nav.take(), NavAction::Next) == 1);
    nav.update(now += 2000, pushed(0.95f));
    CHECK(nav.take().empty());
}

void test_nav_open()
{
    StickNavigator nav;
    nav.reset(0);
    uint32_t now = 5000;
    // Held pressed in near the middle: a bar fills up, open at 0.8 s, once
    nav.update(now, pushed(0.1f, kButtonStick));
    CHECK(std::fabs(nav.openProgress(now + 400) - 0.5f) < 0.01f);
    nav.tick(now + 799);
    CHECK(nav.take().empty());
    nav.tick(now + 800);
    auto got = nav.take();
    CHECK(got.size() == 1 && got[0] == NavAction::Open);
    CHECK(nav.openProgress(now + 900) == 0.0f);
    now += 800;
    CHECK(idle(nav, now, 3000).empty());
    // Let go and pressed again: open again
    nav.update(now += 40, pushed(0.0f));
    CHECK(nav.openProgress(now) == 0.0f);
    nav.update(now += 40, pushed(0.0f, kButtonStick));
    CHECK(count(idle(nav, now, 900), NavAction::Open) == 1);
    // Let go too soon: nothing
    nav.update(now += 40, pushed(0.0f));
    nav.update(now += 40, pushed(0.0f, kButtonStick));
    idle(nav, now, 500);
    nav.update(now += 40, pushed(0.0f));
    CHECK(idle(nav, now, 2000).empty());
    // Pressed in while pushed hard to the side: no press, nor back in the middle still held, and no step either
    nav.update(now += 40, pushed(0.8f, kButtonStick));
    nav.update(now += 40, pushed(0.1f, kButtonStick));
    CHECK(idle(nav, now, 2000).empty());
    // ... let go in the middle, pressed again: open
    nav.update(now += 40, pushed(0.0f));
    nav.update(now += 40, pushed(0.0f, kButtonStick));
    CHECK(count(idle(nav, now, 900), NavAction::Open) == 1);
    // Pushed off the middle while held: no open
    nav.update(now += 40, pushed(0.0f));
    nav.update(now += 40, pushed(0.0f, kButtonStick));
    nav.update(now += 300, pushed(0.7f, kButtonStick));
    CHECK(nav.openProgress(now) == 0.0f);
    nav.update(now += 40, pushed(0.0f, kButtonStick));
    CHECK(idle(nav, now, 2000).empty());
    // The M5 button is not the stick
    nav.update(now += 40, pushed(0.0f));
    nav.update(now += 40, pushed(0.0f, kButtonHome));
    CHECK(idle(nav, now, 2000).empty());
    // The official remote never presses
    StickNavigator plain;
    plain.reset(0);
    plain.update(5000, official(0.0f));
    uint32_t t = 5000;
    CHECK(idle(plain, t, 2000).empty());
    // A batch after a stall: still pressed at 0.9 s, then let go, in one go: the release comes first, no open (tick()
    // looks after the whole batch); held on through the batch: open then
    StickNavigator stall;
    stall.reset(0);
    stall.update(5000, pushed(0.0f, kButtonStick));
    stall.tick(5600);
    stall.update(5900, pushed(0.1f, kButtonStick));
    stall.update(5900, pushed(0.0f));
    stall.tick(5900);
    CHECK(stall.take().empty());
    stall.update(6000, pushed(0.0f, kButtonStick));
    stall.update(6900, pushed(0.1f, kButtonStick));
    CHECK(stall.take().empty());
    stall.tick(6900);
    CHECK(count(stall.take(), NavAction::Open) == 1);
}

void test_nav_first_packet()
{
    // Soon after listening started (within 1 s), the first packet says the stick was pressed in before: no open,
    // until let go and pressed again
    StickNavigator held;
    held.reset(5000);
    uint32_t now = 5500;
    held.update(now, pushed(0.0f, kButtonStick));
    CHECK(held.openProgress(now) == 0.0f);
    CHECK(idle(held, now, 2000).empty());
    held.update(now += 40, pushed(0.0f));
    held.update(now += 40, pushed(0.0f, kButtonStick));
    CHECK(count(idle(held, now, 900), NavAction::Open) == 1);
    // A while after (1 s): the remote was quiet, the press is the first packet: it counts
    StickNavigator quiet;
    quiet.reset(5000);
    now = 6000;
    quiet.update(now, pushed(0.0f, kButtonStick));
    CHECK(count(idle(quiet, now, 900), NavAction::Open) == 1);
    // Pushed out before: no step, nor going on; back and out again: one
    StickNavigator out;
    out.reset(5000);
    now = 5500;
    out.update(now, pushed(0.9f));
    CHECK(idle(out, now, 3000).empty());
    out.update(now += 40, pushed(0.0f));
    out.update(now += 40, pushed(0.9f));
    CHECK(count(out.take(), NavAction::Next) == 1);
    // ... and out there a while after listening started: a step
    StickNavigator late;
    late.reset(5000);
    late.update(6500, pushed(-0.9f));
    CHECK(count(late.take(), NavAction::Previous) == 1);
    // Woken from the screensaver (reset) by the stick pushed out and pressed in, let go out there and held: no step,
    // no going on (it was so before: back to the middle first)
    StickNavigator woken;
    woken.reset(5000);
    woken.update(5500, pushed(0.9f, kButtonStick));
    woken.update(5540, pushed(0.9f));
    now = 5540;
    CHECK(idle(woken, now, 2000).empty());
    woken.update(now += 40, pushed(0.0f));
    woken.update(now += 40, pushed(0.9f));
    CHECK(count(woken.take(), NavAction::Next) == 1);
    // Never reset: whatever the first packet says was so before
    StickNavigator never;
    now = 5000;
    never.update(now, pushed(0.0f, kButtonStick));
    CHECK(idle(never, now, 2000).empty());
    // Reset as a boot (nothing went on before it): the first packet counts however soon it comes
    StickNavigator boot;
    boot.reset(5000, true);
    now = 5100;
    boot.update(now, pushed(0.0f, kButtonStick));
    CHECK(count(idle(boot, now, 900), NavAction::Open) == 1);
    StickNavigator bootOut;
    bootOut.reset(5000, true);
    bootOut.update(5100, pushed(0.9f));
    CHECK(count(bootOut.take(), NavAction::Next) == 1);
    // reset() forgets: a press under way does not open
    StickNavigator again;
    again.reset(0);
    now = 5000;
    again.update(now, pushed(0.0f, kButtonStick));
    again.reset(now + 400);
    CHECK(again.openProgress(now + 500) == 0.0f);
    now += 400;
    CHECK(idle(again, now, 2000).empty());
}

void test_nav_clock_wrap()
{
    // The millisecond clock wraps while the stick is held in, and while it is held out
    StickNavigator nav;
    uint32_t now = 0xFFFFFFFFu - 5000;
    nav.reset(now);
    now = 0xFFFFFFFFu - 200;
    nav.update(now, pushed(0.0f, kButtonStick));
    CHECK(count(idle(nav, now, 900), NavAction::Open) == 1);
    nav.update(now += 40, pushed(0.0f));
    nav.update(now += 40, pushed(1.0f));
    CHECK(count(nav.take(), NavAction::Next) == 1);
    CHECK(count(idle(nav, now, 1100), NavAction::Next) == 2);  // at 0.6 s and 1.05 s
}

}  // namespace

int main()
{
    RUN_TEST(test_packets);
    RUN_TEST(test_shake_and_nod);
    RUN_TEST(test_steering_is_no_gesture);
    RUN_TEST(test_circle);
    RUN_TEST(test_buttons);
    RUN_TEST(test_quiet_time_and_buttons);
    RUN_TEST(test_first_packet);
    RUN_TEST(test_diagonal_is_no_shake);
    RUN_TEST(test_clock_wrap);
    RUN_TEST(test_nav_steps);
    RUN_TEST(test_nav_repeat);
    RUN_TEST(test_nav_open);
    RUN_TEST(test_nav_first_packet);
    RUN_TEST(test_nav_clock_wrap);
    std::cout << "all remote tests passed (" << stackpet_test::checks() << " checks)\n";
    return 0;
}
