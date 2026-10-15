/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
// StackPet: Claude's usage for the launcher's screensaver (clock_screen.h). While the screensaver shows, now and then:
// the remote link paused, the launcher's WiFi (ESP-NOW) onto the router for a moment (Hal::joinRouter(), the robot's
// saved WiFi), GET /api/v1/snapshot from the Mac's ClaudeUsageBar (its LAN service, a bearer token), back on the
// remote's channel. Where the Mac is comes over USB (tools/stackpet_usage_setup.py: a line on the USB-Serial/JTAG
// port, see usage_config.h), kept in NVS. Main loop only; the fetch runs in a task of its own while it lasts
#pragma once
#include "clock_screen.h"
#include "usage_config.h"
#include "usage_look.h"
#include "usage_snapshot.h"
#include <cstdint>
#include <string>

namespace stackpet::usage {

constexpr uint32_t kFetchEveryMs   = 60 * 1000;  // while the screensaver shows
constexpr uint32_t kJoinTimeoutMs  = 8000;       // the first time a full scan, later straight to the router
constexpr uint32_t kHttpTimeoutMs  = 5000;  // the answer, once connected: room for the Mac to send it again
constexpr size_t kMaxResponse      = 16 * 1024;  // the snapshot is about 1.3 KB
constexpr int64_t kClockFixSec     = 2;          // the Mac's clock and ours further apart: ours set from it

class UsageFeed {
public:
    static UsageFeed& instance();

    // The launcher, each frame. wanted: the screensaver shows (a fetch at once unless the last is recent, then every
    // kFetchEveryMs). A fetch that is done hands its result over and resumes the remote link
    void update(uint32_t nowMs, bool wanted);
    // Before an app opens: a fetch on its way is told to stop and waited for (at most about a second and a half),
    // the link resumed: the WiFi is the app's next
    void stop(uint32_t nowMs);
    bool fetching() const;
    // What the screensaver shows: the clock, the usage and how the feed stands
    void fill(ClockModel& m, uint32_t nowMs);
    // The USB console: whatever came since the last call; a whole "stackpet usage ..." line sets, clears or shows
    // where the Mac is (the token never goes to the log), picks the style, or brings the Mac's timezone (taken only
    // while the robot still has the default, GMT0: one picked in SETUP stays)
    void pollConsole(uint32_t nowMs);
    // The screensaver's style, kept in NVS (暖黑 until one is picked: the remote's left and right on the screensaver)
    Style style();
    void setStyle(Style style);

private:
    struct Job;
    static void run_job(void* arg);  // the fetch task
    UsageFeed() = default;
    void load_config();
    void start_job(uint32_t nowMs);
    void finish_job(uint32_t nowMs);
    void handle_line(const std::string& line, uint32_t nowMs);

    bool _loaded = false;
    Config _config;
    Style _style = Style::Claude;
    Job* _job = nullptr;  // the fetch on its way (its task's until done)
    bool _wanted     = false;
    uint32_t _next_ms = 0;
    bool _have        = false;  // a snapshot came (this boot)
    Snapshot _snapshot;
    uint32_t _snapshot_ms = 0;
    FeedStatus _status    = FeedStatus::Waiting;
    std::string _line;  // the console line so far
    bool _line_too_long = false;
};

}  // namespace stackpet::usage
