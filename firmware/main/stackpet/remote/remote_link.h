/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "remote_packet.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <vector>

namespace stackpet::remote {

// The apps that keep listening when opened while the remote is in use, so that its M5 button takes them home again:
// the ones that need neither the network nor BLE for themselves (launcher names)
bool appKeepsListening(std::string_view appName);
// The ones that connect to the router as they open: the link listens on their WiFi once it is up (attach())
bool appListensOnItsWifi(std::string_view appName);

/**
 * The StackChan remote (ESP-NOW broadcasts, see remote_packet.h), listened for from boot on:
 * - the launcher starts listening on the saved channel, with a look at one other channel in turn every few seconds,
 *   until the remote is heard; then it stays on the remote's channel (the frame says which, also when it was heard
 *   a few channels off, close to the remote); the stick goes through the apps there (stick_navigator.h);
 * - a remote heard on another channel than its own (close to it) is followed there, at most every few seconds;
 * - NVS keeps the channel the remote was last heard on: looked at first next time;
 * - NVS keeps the receiver id too (the remote control app's advanced page): every listener filters by it;
 * - the apps that do not need the WiFi for themselves (appKeepsListening()) keep listening when the remote opens them,
 *   or when they are touched open once the remote has been used (usedSincePowerOn()): there the M5 button goes back to
 *   the home screen (takeHomePress(), the home indicator asks);
 * - every other app gets the WiFi back first (release()), they start it their own way; once it is up (connected to the
 *   router), the link listens on it again if the remote is in use (attach()): only the M5 button's copy sent on every
 *   channel is heard there, enough to go home;
 * Main loop only, except the ESP-NOW receive callback: it queues the packets and notes the M5 button (guarded by
 * _mutex), and reads the receiver id (atomic); and attach(): it runs where the app's WiFi comes up (WEB.REMOTE's
 * network task, the AI agent's stackchan task), while the main loop may ask listening() / takeHomePress() (atomic).
 */
class RemoteLink {
public:
    static RemoteLink& instance();

    void start(bool hop);  // listen (hop: also look for the remote on the other channels until it is heard)
    // An app's WiFi is up: listen on it too (its channel, the router's), if the remote has been used since power-on
    void attach();
    void release();        // stop listening, the WiFi off again (attached: the app's WiFi stays)
    bool listening() const;
    int channel() const;  // 0: not listening
    void lockChannel(int channel);
    // An app keeps listening (it never calls update()): no more looking round the channels, on the saved one (where
    // the remote was heard last) unless on the remote's already
    void settle();

    // Every tick: looks for the remote while hopping; the packets for this robot since the last call (oldest first)
    std::vector<RemotePacket> update(uint32_t nowMs);

    // Away for a moment: the launcher's WiFi on the router (the screensaver fetching Claude's usage, the router's
    // channel meanwhile). No looking round or following, no channel changes; the packets still come in (those heard
    // there: the M5 button's copies, a remote close by). A look at another channel cut short: channel() is the saved
    // one, where the fetch goes back to. resume(): looking round again from nowMs on
    void pause();
    void resume(uint32_t nowMs);
    bool paused() const;

    // The receiver id this robot answers to besides 0 (every robot); 1 unless the advanced page picked another
    int receiverId();
    void setReceiverId(int id);

    // The M5 button pressed on this robot's remote since the last call, at most a second ago (a press nobody asked
    // about in time is dropped, not kept for later). Whoever asks first gets it.
    bool takeHomePress(uint32_t nowMs);
    // StackPet's remote (it has the M5 button: the official one does not) heard since the robot was powered on, over
    // the warm reboot every app closes with: the apps touched open keep listening then too
    static bool usedSincePowerOn();

    // The launcher opened the remote control app with the remote: no role page, receiver right away
    void setDirectReceiver(bool direct);
    bool takeDirectReceiver();

private:
    RemoteLink() = default;
    void connect_once();
    void forget_received();  // the queue and the M5 button's state, before listening again
    void on_data(const std::vector<uint8_t>& data);
    void switch_channel(int channel);

    std::mutex _mutex;
    std::vector<RemotePacket> _queue;  // received, not yet taken (guarded)
    bool _home_down       = false;     // the M5 button held in the last packet for us (guarded) ...
    bool _home_pressed    = false;     // ... pressed since, at _home_ms (guarded) ...
    bool _home_copy       = false;     // ... heard in its copy on every channel, else sent on _home_sent_on
    int _home_sent_on     = 0;
    uint32_t _home_ms     = 0;
    int _home_count       = -1;        // the press count in the last packet that had one (guarded)
    bool _connected       = false;
    std::atomic<bool> _listening{false};
    bool _attached        = false;  // on an app's WiFi (attach()): its channel, no looking round or following
    bool _paused          = false;  // the launcher's WiFi on the router for a moment (pause())
    bool _hopping         = false;  // looking for the remote (the launcher)
    bool _probing         = false;  // looking on _probe_channel for a moment, back on the saved one after
    int _channel          = 0;
    int _saved_channel    = 0;   // what NVS has
    std::atomic<int> _receiver_id{-1};  // what NVS has, -1: not read yet (the receive callback reads it too)
    int _probe_channel    = 0;
    uint32_t _probe_ms    = 0;      // the last switch to or from a probe channel
    bool _followed        = false;  // on the remote's channel since _followed_ms
    uint32_t _followed_ms = 0;
    bool _stick_pressed   = false;  // for the log
    bool _mismatch_logged = false;
    uint32_t _log_ms      = 0;      // the packets' summary: the last one at, counts since, the last packet for us
    int _log_heard        = 0;
    int _log_ours         = 0;
    RemotePacket _log_last;
    bool _direct_receiver = false;
};

}  // namespace stackpet::remote
