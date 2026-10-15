/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
// Host tests of RemoteLink (stackpet/remote/remote_link.cpp) with stand-ins for the HAL, NVS and the log
// (tests/stubs_remote)
#include "stackpet_games_check.h"
#include <hal/hal.h>
#include <mooncake_log.h>
#include <settings.h>
#include <stackpet/remote/remote_link.h>
#include <algorithm>
#include <iostream>
#include <set>
#include <string>
#include <vector>

using namespace stackpet::remote;

namespace {

// The remote's packet: 8 bytes (the official remote), 9 with the buttons, 10 with the channel it sends on too, 11 with
// the M5 button's count
std::vector<uint8_t> packet(uint8_t target, int16_t yaw, int buttons = -1, int channel = -1, int count = -1)
{
    const int16_t pitch = 450;
    const int16_t speed = 600;
    std::vector<uint8_t> b = {target,
                              static_cast<uint8_t>(yaw & 0xFF),
                              static_cast<uint8_t>((yaw >> 8) & 0xFF),
                              static_cast<uint8_t>(pitch & 0xFF),
                              static_cast<uint8_t>((pitch >> 8) & 0xFF),
                              static_cast<uint8_t>(speed & 0xFF),
                              static_cast<uint8_t>((speed >> 8) & 0xFF),
                              0};
    if (buttons >= 0) {
        b.push_back(static_cast<uint8_t>(buttons));
    }
    if (channel >= 0) {
        b.push_back(static_cast<uint8_t>(channel));
    }
    if (count >= 0) {
        b.push_back(static_cast<uint8_t>(count));
    }
    return b;
}

// Not listening, nothing in NVS (release() makes the link read the receiver id from NVS again)
void fresh(uint32_t now = 1000)
{
    RemoteLink::instance().release();
    GetHostNvs() = HostNvs{};
    GetHAL().now = now;
}

// A tick every stepMs for ms; the channel listened on after each tick
std::vector<int> run(uint32_t ms, uint32_t stepMs = 50)
{
    std::vector<int> channels;
    auto& hal = GetHAL();
    for (uint32_t t = 0; t < ms; t += stepMs) {
        hal.now += stepMs;
        RemoteLink::instance().update(hal.now);
        channels.push_back(RemoteLink::instance().channel());
    }
    return channels;
}

int saved(const char* key, int missing = -1)
{
    const auto it = GetHostNvs().ints.find(std::string("stackpet_remote/") + key);
    return it == GetHostNvs().ints.end() ? missing : it->second;
}

void test_looking_for_the_remote()
{
    fresh();
    auto& link = RemoteLink::instance();
    link.start(true);
    CHECK(link.listening() && link.channel() == 1);  // nothing saved: the remote's default
    // 40 s with nothing heard: nearly always on the saved channel, every other one looked at, never long away
    const auto channels = run(40000);
    int home         = 0;
    int away         = 0;
    int longest_away = 0;
    std::set<int> others;
    for (int c : channels) {
        if (c == 1) {
            home++;
            away = 0;
        } else {
            others.insert(c);
            longest_away = std::max(longest_away, ++away);
        }
    }
    CHECK(home * 100 >= static_cast<int>(channels.size()) * 93);
    CHECK(others.size() == 12);
    CHECK(longest_away * 50 <= 200);
    CHECK(GetHostNvs().writes == 0);
}

void test_saved_channel_first()
{
    fresh();
    GetHostNvs().ints["stackpet_remote/channel"] = 9;
    auto& link = RemoteLink::instance();
    link.start(true);
    CHECK(link.channel() == 9);
    const auto channels = run(10000);
    CHECK(std::count(channels.begin(), channels.end(), 9) * 100 >= static_cast<int>(channels.size()) * 93);
    // Nonsense in NVS: the default
    fresh();
    GetHostNvs().ints["stackpet_remote/channel"] = 77;
    link.start(true);
    CHECK(link.channel() == 1);
}

void test_heard_while_looking()
{
    // StackPet's remote, heard a few channels off (close to it): on its channel, saved, no more looking
    fresh();
    auto& link = RemoteLink::instance();
    link.start(true);
    GetHAL().receive(packet(0, 100, 0, 6), 6);
    GetHAL().now += 50;
    const auto got = link.update(GetHAL().now);
    CHECK(got.size() == 1 && got[0].yaw == 100 && got[0].sentOn == 6 && got[0].channel == 6);
    CHECK(link.channel() == 6 && saved("channel") == 6 && GetHostNvs().writes == 1);
    auto channels = run(10000);
    CHECK(std::all_of(channels.begin(), channels.end(), [](int c) { return c == 6; }));
    // More of it there: nothing more written
    for (int i = 0; i < 5; i++) {
        GetHAL().receive(packet(0, 100, 0, 6), 6);
        run(100);
    }
    CHECK(GetHostNvs().writes == 1);

    // The official remote (8 bytes) heard on channel 1 though it sends on 3: the frame header says 3
    fresh();
    link.start(true);
    GetHAL().receive(packet(0, -200), 3);
    run(50);
    CHECK(link.channel() == 3 && saved("channel") == 3);
}

void test_receiver_ids()
{
    fresh();
    auto& link = RemoteLink::instance();
    CHECK(link.receiverId() == 1);
    link.start(true);
    // For robot 2 (on 6) and for every robot (on 3): only the second one is ours, and it decides the channel
    GetHAL().receive(packet(2, 100, 0, 6), 6);
    GetHAL().receive(packet(0, 200, 0, 3), 3);
    GetHAL().now += 50;
    auto got = link.update(GetHAL().now);
    CHECK(got.size() == 1 && got[0].yaw == 200);
    CHECK(link.channel() == 3);
    // Robot 2 from now on (the remote control app's advanced page): kept in NVS
    link.setReceiverId(2);
    CHECK(link.receiverId() == 2 && saved("id") == 2);
    GetHAL().receive(packet(2, 300, 0, 3), 3);
    GetHAL().receive(packet(1, 400, 0, 3), 3);
    GetHAL().now += 50;
    got = link.update(GetHAL().now);
    CHECK(got.size() == 1 && got[0].yaw == 300);
    // No such receiver
    link.setReceiverId(0);
    link.setReceiverId(300);
    CHECK(link.receiverId() == 2);
    // Kept over a boot: what NVS says
    fresh();
    GetHostNvs().ints["stackpet_remote/id"] = 5;
    CHECK(link.receiverId() == 5);
    fresh();
    GetHostNvs().ints["stackpet_remote/id"] = 999;  // nonsense: the default
    CHECK(link.receiverId() == 1);
}

void test_channel_sources()
{
    // The frame header says 3, the packet's own byte 6 (they should not differ): the frame's
    fresh();
    auto& link = RemoteLink::instance();
    link.start(true);
    GetHAL().receive(packet(0, 0, 0, 6), 3);
    run(50);
    CHECK(link.channel() == 3 && saved("channel") == 3);
    // A remote on channel 14 (heard a channel off, close to it): not ours to follow, nothing handed out
    fresh();
    link.start(true);
    GetHAL().receive(packet(0, 0, 0, 14), 14);
    GetHAL().receive(packet(0, 0), 14);
    GetHAL().now += 50;
    CHECK(link.update(GetHAL().now).empty());
    const auto channels = run(4000);
    CHECK(std::any_of(channels.begin(), channels.end(), [](int c) { return c != 1; }));  // still looking
    CHECK(saved("channel") == -1);
}

void test_following_the_remote()
{
    fresh();
    auto& link = RemoteLink::instance();
    link.start(true);
    GetHAL().receive(packet(0, 0, 0, 1), 1);
    run(50);
    CHECK(link.channel() == 1 && GetHostNvs().writes == 0);  // the default: nothing to write
    // A second later it says 6 (another remote, or this one moved): not yet, at most every 3 s
    run(1000);
    GetHAL().receive(packet(0, 0, 0, 6), 6);
    run(50);
    CHECK(link.channel() == 1);
    run(2500);
    GetHAL().receive(packet(0, 0, 0, 6), 6);
    run(50);
    CHECK(link.channel() == 6 && saved("channel") == 6 && GetHostNvs().writes == 1);
    // Two remotes for every robot, on 1 and on 6, both heard: pulled to and fro at most every 3 s
    const int writes = GetHostNvs().writes;
    int switches     = 0;
    int last         = link.channel();
    for (int i = 0; i < 100; i++) {
        GetHAL().receive(packet(0, 0, 0, i % 2 == 0 ? 1 : 6), i % 2 == 0 ? 1 : 6);
        run(100);
        if (link.channel() != last) {
            switches++;
            last = link.channel();
        }
    }
    CHECK(switches <= 4);
    CHECK(GetHostNvs().writes - writes <= 4);
    // Locked on another channel by hand (the advanced page): the remote's own wins once it is heard
    fresh();
    link.start(false);
    link.lockChannel(11);
    GetHAL().receive(packet(0, 0, 0, 4), 4);
    run(50);
    CHECK(link.channel() == 4);
}

void test_home_press()
{
    fresh();
    auto& link = RemoteLink::instance();
    auto& hal  = GetHAL();
    // Not listening: nothing heard
    hal.receive(packet(0, 0, kButtonHome, 1), 1);
    CHECK(!link.takeHomePress(hal.now));
    link.start(true);
    // Pressed: once, to whoever asks first, also when nobody takes the packets
    hal.receive(packet(0, 0, kButtonHome, 1), 1);
    CHECK(link.takeHomePress(hal.now));
    CHECK(!link.takeHomePress(hal.now));
    // Held (more packets with it down, the stick moving meanwhile): no new press; let go, pressed again: one
    hal.receive(packet(0, 100, kButtonHome, 1), 1);
    hal.receive(packet(0, 200, kButtonHome, 1), 1);
    CHECK(!link.takeHomePress(hal.now));
    hal.receive(packet(0, 200, 0, 1), 1);
    hal.receive(packet(0, 200, kButtonHome, 1), 1);
    CHECK(link.takeHomePress(hal.now));
    // With the stick pressed in too: still the M5 button; the stick alone is not
    hal.receive(packet(0, 0, 0, 1), 1);
    hal.receive(packet(0, 0, kButtonStick | kButtonHome, 1), 1);
    CHECK(link.takeHomePress(hal.now));
    hal.receive(packet(0, 0, 0, 1), 1);
    hal.receive(packet(0, 0, kButtonStick, 1), 1);
    CHECK(!link.takeHomePress(hal.now));
    // Asked more than a second later: stale, gone
    hal.receive(packet(0, 0, 0, 1), 1);
    hal.receive(packet(0, 0, kButtonHome, 1), 1);
    CHECK(!link.takeHomePress(hal.now + 1001));
    CHECK(!link.takeHomePress(hal.now));
    // Asked with the clock read just before it came in: fresh
    hal.receive(packet(0, 0, 0, 1), 1);
    hal.now += 10;
    hal.receive(packet(0, 0, kButtonHome, 1), 1);
    CHECK(link.takeHomePress(hal.now - 10));
    // Another robot's (id 2), or from channel 14: not ours
    hal.receive(packet(0, 0, 0, 1), 1);
    hal.receive(packet(2, 0, kButtonHome, 1), 1);
    CHECK(!link.takeHomePress(hal.now));
    hal.receive(packet(0, 0, kButtonHome, 14), 14);
    CHECK(!link.takeHomePress(hal.now));
    // ... robot 2's once this one is robot 2 (the advanced page)
    link.setReceiverId(2);
    hal.receive(packet(2, 0, kButtonHome, 1), 1);
    CHECK(link.takeHomePress(hal.now));
    // The packets themselves are handed out as before, the button in them
    hal.now += 50;
    const auto got = link.update(hal.now);
    CHECK(!got.empty() && got.back().target == 2 && got.back().buttons == kButtonHome);
    // The receiver id comes from NVS before anything is heard (start() reads it), not the default
    fresh();
    GetHostNvs().ints["stackpet_remote/id"] = 5;
    link.start(false);
    hal.receive(packet(1, 0, kButtonHome, 1), 1);
    CHECK(!link.takeHomePress(hal.now));
    hal.receive(packet(5, 0, kButtonHome, 1), 1);
    CHECK(link.takeHomePress(hal.now));
    // Released: a press not yet asked about is dropped; listening again, a button held since is a press when heard
    hal.receive(packet(0, 0, 0, 1), 1);
    hal.receive(packet(0, 0, kButtonHome, 1), 1);
    link.release();
    CHECK(!link.takeHomePress(hal.now));
    link.start(false);
    CHECK(!link.takeHomePress(hal.now));
    hal.receive(packet(0, 0, kButtonHome, 1), 1);
    CHECK(link.takeHomePress(hal.now));
    // The official remote (8 bytes, no buttons): never
    hal.receive(packet(0, 0), 1);
    hal.receive(packet(0, 300), 1);
    CHECK(!link.takeHomePress(hal.now));
}

void test_listening_apps()
{
    // The apps that need neither the network nor BLE for themselves, by their launcher names
    for (const char* name : {"ESPNOW.REMOTE", "RADAR", "FACE", "SPIN", "FREEZE"}) {
        CHECK(appKeepsListening(name));
    }
    for (const char* name : {"AI.AGENT", "AVATAR", "APP.CENTER", "EZDATA", "DANCE", "SETUP", "WEB.REMOTE", "", "radar"}) {
        CHECK(!appKeepsListening(name));
    }
    // A name that outlives nothing it was made from: a copy is compared, not where it came from
    std::string name = "RADAR";
    const bool radar = appKeepsListening(name);
    name             = "SETUP";
    CHECK(radar && !appKeepsListening(name));
}

void test_settle()
{
    fresh();
    auto& link = RemoteLink::instance();
    link.settle();  // not listening: nothing
    CHECK(!link.listening());
    GetHostNvs().ints["stackpet_remote/channel"] = 4;
    link.start(true);
    // Looking round (every 3 s), caught on another channel: settled, back on the saved one, no more looking
    std::vector<int> channels;
    for (int i = 0; i < 500 && channels.empty(); i++) {
        GetHAL().now += 10;
        link.update(GetHAL().now);
        if (link.channel() != 4) {
            channels.push_back(link.channel());
        }
    }
    CHECK(!channels.empty());
    link.settle();
    CHECK(link.channel() == 4);
    channels = run(10000);
    CHECK(std::all_of(channels.begin(), channels.end(), [](int c) { return c == 4; }));
    // On the remote's channel already, or locked by hand: settle() leaves it there
    GetHAL().receive(packet(0, 0, 0, 7), 7);
    run(50);
    CHECK(link.channel() == 7);
    link.settle();
    CHECK(link.channel() == 7);
    link.lockChannel(9);
    link.settle();
    CHECK(link.channel() == 9);
}

// Runs first: until a packet of StackPet's remote comes in, nothing has used it since "power-on" (the test run)
void test_used_since_power_on()
{
    fresh();
    auto& link = RemoteLink::instance();
    auto& hal  = GetHAL();
    CHECK(!RemoteLink::usedSincePowerOn());
    // Not in use: no listening on an app's WiFi either
    hal.wifi_up = true;
    link.attach();
    CHECK(!link.listening() && hal.attaches == 0);
    hal.wifi_up = false;
    link.start(true);
    // The official remote (no buttons byte: no M5 button either), another robot's: not this one in use
    hal.receive(packet(0, 100), 1);
    hal.receive(packet(2, 100, 0, 1), 1);
    run(50);
    CHECK(!RemoteLink::usedSincePowerOn());
    // StackPet's remote for this robot: in use, also before update() hands it out, and over release() / start()
    hal.receive(packet(0, 100, 0, 1), 1);
    CHECK(RemoteLink::usedSincePowerOn());
    link.release();
    CHECK(RemoteLink::usedSincePowerOn());
    link.start(true);
    CHECK(RemoteLink::usedSincePowerOn());
}

void test_home_press_count()
{
    fresh();
    auto& link            = RemoteLink::instance();
    auto& hal             = GetHAL();
    const uint8_t copy    = kButtonHome | kButtonEverywhere;
    link.start(true);
    // A press by its count: the packet on the remote's channel and its copies on every channel are one press
    hal.receive(packet(0, 0, 0, 1, 4), 1);
    hal.receive(packet(0, 0, kButtonHome, 1, 5), 1);
    hal.receive(packet(0, 0, copy, 1, 5), 1);
    hal.receive(packet(0, 0, copy, 1, 5), 1);
    CHECK(link.takeHomePress(hal.now));
    hal.receive(packet(0, 0, copy, 1, 5), 1);
    CHECK(!link.takeHomePress(hal.now));
    // Its release never heard (only copies come on an app's WiFi): the next press counts all the same; round the wrap
    hal.receive(packet(0, 0, copy, 1, 6), 1);
    CHECK(link.takeHomePress(hal.now));
    hal.receive(packet(0, 0, copy, 1, 255), 1);
    CHECK(link.takeHomePress(hal.now));
    hal.receive(packet(0, 0, copy, 1, 0), 1);
    CHECK(link.takeHomePress(hal.now));
    // The copies are not handed out (they say nothing of where the remote is): only the two ordinary packets
    hal.now += 50;
    const auto got = link.update(hal.now);
    CHECK(got.size() == 2 && got[0].homeCount == 4 && got[1].homeCount == 5);
    // A copy heard here on 1 whose frame says 9 (the component leaves the receiver's own channel in a copy's rx_ctrl,
    // so this is the worse case): not followed there
    hal.receive(packet(0, 0, copy, 1, 7), 9);
    CHECK(link.takeHomePress(hal.now));
    const auto channels = run(4000);
    CHECK(std::all_of(channels.begin(), channels.end(), [](int c) { return c == 1; }));
    // Another robot's copy: not ours
    hal.receive(packet(2, 0, copy, 1, 8), 1);
    CHECK(!link.takeHomePress(hal.now));
    // Listening again: whatever count came before, the first press heard counts
    link.release();
    link.start(true);
    hal.receive(packet(0, 0, copy, 1, 8), 1);
    CHECK(link.takeHomePress(hal.now));
}

void test_home_log()
{
    fresh();
    auto& link         = RemoteLink::instance();
    auto& hal          = GetHAL();
    const uint8_t copy = kButtonHome | kButtonEverywhere;
    link.start(true);
    // Which way the press came, in the log: its copy on every channel, or the ordinary packet (sent on its channel)
    hal.receive(packet(0, 0, copy, 1, 20), 1);
    CHECK(link.takeHomePress(hal.now));
    CHECK(mclog::lastInfo() == "M5 button: home (its copy on every channel)");
    CHECK(mclog::lastInfoValues().empty());
    // Sent on another channel than the one listened on (heard a few channels off, close by): that one in the line
    hal.receive(packet(0, 0, 0, 3, 20), 3);
    hal.receive(packet(0, 0, kButtonHome, 3, 21), 3);
    CHECK(link.takeHomePress(hal.now));
    CHECK(mclog::lastInfo() == "M5 button: home (sent on {})");
    CHECK(mclog::lastInfoValues() == std::vector<long long>{3});
    // The ordinary packet first, its copies after: one press, said as the ordinary one
    hal.receive(packet(0, 0, kButtonHome, 1, 22), 1);
    hal.receive(packet(0, 0, copy, 1, 22), 1);
    hal.receive(packet(0, 0, copy, 1, 22), 1);
    CHECK(link.takeHomePress(hal.now));
    CHECK(mclog::lastInfo() == "M5 button: home (sent on {})");
    CHECK(mclog::lastInfoValues() == std::vector<long long>{1});
    CHECK(!link.takeHomePress(hal.now));
    // A copy first: said as the copy, the ordinary one after it changes nothing
    hal.receive(packet(0, 0, copy, 1, 23), 1);
    hal.receive(packet(0, 0, kButtonHome, 1, 23), 1);
    CHECK(link.takeHomePress(hal.now));
    CHECK(mclog::lastInfo() == "M5 button: home (its copy on every channel)");
    // Nothing taken: no line
    mclog::lastInfo().clear();
    CHECK(!link.takeHomePress(hal.now));
    CHECK(mclog::lastInfo().empty());
}

void test_pause()
{
    fresh();
    auto& link = RemoteLink::instance();
    auto& hal  = GetHAL();
    link.start(true);
    const int saved = link.channel();
    // Caught looking at another channel: paused, the link's channel is the saved one again (the fetch returns there)
    int guard = 0;
    while (link.channel() == saved && guard++ < 400) {
        run(50);
    }
    CHECK(link.channel() != saved);
    link.pause();
    CHECK(link.paused());
    CHECK(link.channel() == saved);
    // Paused: no channel changes however long; the packets still come in, one from another channel is not followed
    hal.channel    = 2;  // the router's (joinRouter())
    const int sets = hal.channel_sets;
    run(20000);
    CHECK(hal.channel_sets == sets);
    hal.receive(packet(0, 300, 0, 5), 5);
    CHECK(link.update(hal.now).size() == 1);
    CHECK(hal.channel_sets == sets && link.channel() == saved);
    // The M5 button's copies are heard there
    hal.receive(packet(0, 0, kButtonHome | kButtonEverywhere, 1, 40), 2);
    CHECK(link.takeHomePress(hal.now));
    // Resumed (leaveRouter() put the radio back): looking round again, the next look a while from now
    hal.channel = saved;
    link.resume(hal.now);
    CHECK(!link.paused());
    const auto soon = run(1000);
    CHECK(std::all_of(soon.begin(), soon.end(), [saved](int c) { return c == saved; }));
    guard = 0;
    while (link.channel() == saved && guard++ < 400) {
        run(50);
    }
    CHECK(link.channel() != saved);
    // Heard on its channel after: found and locked as usual
    hal.receive(packet(0, 300, 0, 3), 3);
    link.update(hal.now);
    CHECK(link.channel() == 3);
    // Not listening, attached: nothing to pause; release() and start() end a pause
    link.pause();
    CHECK(link.paused());
    link.release();
    CHECK(!link.paused());
    link.pause();
    CHECK(!link.paused());
    link.start(true);
    link.pause();
    link.start(true);  // already listening: start() does nothing
    CHECK(link.paused());
    link.resume(hal.now);
    CHECK(!link.paused());
    link.resume(hal.now);
    CHECK(!link.paused());
}

void test_attach()
{
    // The earlier tests heard StackPet's remote: in use since "power-on"
    fresh();
    auto& link = RemoteLink::instance();
    auto& hal  = GetHAL();
    CHECK(RemoteLink::usedSincePowerOn());
    // The app's WiFi not up: nothing
    hal.wifi_up = false;
    link.attach();
    CHECK(!link.listening());
    // Up, on its router's channel 6: listening there, no looking round, following or saving, whatever is heard
    hal.wifi_up    = true;
    hal.ap_channel = 6;
    const int sets = hal.channel_sets;
    link.attach();
    CHECK(link.listening() && link.channel() == 6);
    hal.receive(packet(0, 0, 0, 1, 3), 1);  // close by, from the remote's channel 1 (heard a few channels off)
    const auto channels = run(10000);
    CHECK(std::all_of(channels.begin(), channels.end(), [](int c) { return c == 6; }));
    link.lockChannel(2);
    link.settle();
    CHECK(link.channel() == 6 && hal.channel_sets == sets && saved("channel") == -1);
    // The M5 press's copy: home
    hal.receive(packet(0, 0, kButtonHome | kButtonEverywhere, 1, 4), 6);
    CHECK(link.takeHomePress(hal.now));
    // Attached already: again nothing new; start() neither
    const int attaches = hal.attaches;
    const int starts   = hal.starts;
    link.attach();
    link.start(true);
    CHECK(hal.attaches == attaches && hal.starts == starts && link.channel() == 6);
    // Released: the HAL stops it
    link.release();
    CHECK(!link.listening() && !hal.started);
    hal.wifi_up = false;
}

void test_queue_limit()
{
    fresh();
    auto& link = RemoteLink::instance();
    link.start(false);
    for (int i = 0; i < 33; i++) {
        GetHAL().receive(packet(0, static_cast<int16_t>(i)), 1);
    }
    GetHAL().now += 50;
    const auto got = link.update(GetHAL().now);
    CHECK(got.size() == 32 && got.front().yaw == 1 && got.back().yaw == 32);  // the oldest one dropped
    // Not the remote's: nothing
    GetHAL().receive({1, 2, 3}, 1);
    GetHAL().now += 50;
    CHECK(link.update(GetHAL().now).empty());
}

void test_clock_wrap()
{
    fresh(0xFFFFF000u);  // 4 s before the millisecond clock wraps
    auto& link = RemoteLink::instance();
    link.start(true);
    const auto channels = run(10000);
    int looks = 0;
    for (size_t i = 1; i < channels.size(); i++) {
        looks += channels[i] != 1 && channels[i - 1] == 1 ? 1 : 0;
    }
    CHECK(looks == 3);  // at +3 s, +6.15 s (after the wrap) and +9.3 s
}

void test_start_and_release()
{
    fresh();
    auto& link       = RemoteLink::instance();
    const int starts = GetHAL().starts;
    link.start(true);
    link.start(false);  // already listening: as it was (still looking)
    CHECK(GetHAL().starts == starts + 1);
    auto channels = run(4000);
    CHECK(std::any_of(channels.begin(), channels.end(), [](int c) { return c != 1; }));
    // Locked: no more looking
    link.lockChannel(5);
    channels = run(10000);
    CHECK(std::all_of(channels.begin(), channels.end(), [](int c) { return c == 5; }));
    // Released: the WiFi off, nothing more
    const int stops = GetHAL().stops;
    link.release();
    CHECK(!link.listening() && link.channel() == 0 && GetHAL().stops == stops + 1 && !GetHAL().started);
    GetHAL().receive(packet(0, 0), 5);
    GetHAL().now += 50;
    CHECK(link.update(GetHAL().now).empty());
    link.release();  // twice: nothing
    CHECK(GetHAL().stops == stops + 1);
    // Listening again: what came in meanwhile is not handed out
    GetHAL().receive(packet(0, 123), 1);
    link.start(false);
    GetHAL().now += 50;
    CHECK(link.update(GetHAL().now).empty());
}

}  // namespace

int main()
{
    RUN_TEST(test_used_since_power_on);  // first: no packet of StackPet's remote before it
    RUN_TEST(test_looking_for_the_remote);
    RUN_TEST(test_saved_channel_first);
    RUN_TEST(test_heard_while_looking);
    RUN_TEST(test_receiver_ids);
    RUN_TEST(test_channel_sources);
    RUN_TEST(test_following_the_remote);
    RUN_TEST(test_home_press);
    RUN_TEST(test_listening_apps);
    RUN_TEST(test_settle);
    RUN_TEST(test_home_press_count);
    RUN_TEST(test_home_log);
    RUN_TEST(test_pause);
    RUN_TEST(test_attach);
    RUN_TEST(test_queue_limit);
    RUN_TEST(test_clock_wrap);
    RUN_TEST(test_start_and_release);
    std::cout << "all remote link tests passed (" << stackpet_test::checks() << " checks)\n";
    return 0;
}
