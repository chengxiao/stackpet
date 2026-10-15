/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
// Host tests of the screensaver's Claude usage: the snapshot and HTTP parsing (stackpet/usage/usage_snapshot.cpp),
// its looks (usage_look.cpp) and the console line that sets it up (usage_config.cpp)
#include "stackpet_games_check.h"
#include <stackpet/usage/usage_config.h>
#include <stackpet/usage/usage_look.h>
#include <stackpet/usage/usage_snapshot.h>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <string>

using namespace stackpet::usage;

namespace {

// The shape ClaudeUsageBar answers with (GET /api/v1/snapshot), the parts the screensaver does not use cut short
const char* const kSnapshot = R"({
  "v": 1, "seq": 257190, "ts": 1791892556, "name": "Mac mini",
  "claude": {"session": {"pct": 2, "resetsAt": 1791909599}, "weekly": {"pct": 86, "resetsAt": 1792047599},
             "extra": null, "model": "Opus 5.5", "contextPct": 8, "costUSD": 0.9, "dataAgeSec": 0,
             "tokens24h": 1038032359, "hourlyTokens": [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
             20, 21, 22, 23, 24], "messages24h": 3539},
  "chatgpt": {"available": true, "plan": "prolite", "session": {"pct": null, "resetsAt": null},
              "weekly": {"pct": 38, "resetsAt": 1792067358}},
  "agent": {"state": "work", "agent": "claude", "tool": "Bash", "detail": "build", "project": "firmware",
            "elapsedSec": 283, "sessions": 3, "waiting": 0},
  "mac": {"cpuPct": 27.6, "memTotalBytes": 25769803776},
  "display": {"connected": true}
})";

constexpr int64_t kNow = 1791892556;  // Tue 2026-10-13 19:55:56 in UTC+8

std::tm at(int year, int month, int day, int hour, int minute, int wday)
{
    std::tm t{};
    t.tm_year = year - 1900;
    t.tm_mon  = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min  = minute;
    t.tm_wday = wday;
    return t;
}

void test_snapshot()
{
    Snapshot s;
    CHECK(parseSnapshot(kSnapshot, s));
    CHECK(s.session.pct == 2);
    CHECK(s.session.resetsAt == 1791909599);
    CHECK(s.weekly.pct == 86);
    CHECK(s.weekly.resetsAt == 1792047599);
    CHECK(s.state == AgentState::Work);
    CHECK(s.waiting == 0);
    CHECK(s.ts == kNow);
    CHECK(s.dataAgeSec == 0);
    CHECK(s.model == "Opus 5.5");

    // Not known: null, missing, the wrong type
    CHECK(parseSnapshot(R"({"claude": {"session": {"pct": null}, "weekly": {"pct": "86"}}, "agent": {}})", s));
    CHECK(s.session.pct == -1 && s.session.resetsAt == 0);
    CHECK(s.weekly.pct == -1);
    CHECK(s.state == AgentState::Unknown && s.waiting == 0 && s.ts == 0 && s.dataAgeSec == -1 && s.model.empty());

    // Clamped and rounded
    CHECK(parseSnapshot(R"({"claude": {"session": {"pct": 104, "resetsAt": -5}, "weekly": {"pct": 12.6}},
                            "agent": {"state": "wait", "waiting": 250}})",
                        s));
    CHECK(s.session.pct == 100 && s.session.resetsAt == 0);
    CHECK(s.weekly.pct == 13);
    CHECK(s.state == AgentState::Wait && s.waiting == 99);
    CHECK(parseSnapshot(R"({"claude": {"session": {"pct": -3}}, "agent": {"state": "done"}})", s));
    CHECK(s.session.pct == 0 && s.state == AgentState::Done);
    CHECK(parseSnapshot(R"({"claude": {}, "agent": {"state": "idle"}})", s) && s.state == AgentState::Idle);
    CHECK(parseSnapshot(R"({"claude": {}, "agent": {"state": "sleeping"}})", s) && s.state == AgentState::Unknown);

    // The model: printable ASCII only, at most kModelMax
    CHECK(parseSnapshot(R"({"claude": {"model": "Opusé 5\n.5"}})", s));
    CHECK(s.model == "Opus 5.5");
    CHECK(parseSnapshot(R"({"claude": {"model": "abcdefghijklmnopqrstuvwxyz0123456789"}})", s));
    CHECK(s.model.size() == kModelMax);

    // Not a snapshot: out as it was
    Snapshot kept;
    kept.session.pct = 42;
    CHECK(!parseSnapshot("", kept));
    CHECK(!parseSnapshot("not json", kept));
    CHECK(!parseSnapshot(R"({"claude": 5})", kept));
    CHECK(!parseSnapshot(R"({"chatgpt": {}})", kept));
    CHECK(!parseSnapshot(R"([1, 2])", kept));
    CHECK(!parseSnapshot(R"({"claude": {"session": )", kept));
    CHECK(kept.session.pct == 42);
}

void test_http()
{
    HttpResponse r;
    const std::string ok = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 7\r\n"
                           "Connection: close\r\n\r\n{\"a\":1}";
    CHECK(parseHttpResponse(ok, r));
    CHECK(r.status == 200 && r.body == "{\"a\":1}");
    // Content-Length shorter than what came: only that much; any case
    CHECK(parseHttpResponse("HTTP/1.0 200 OK\r\ncontent-length:  2\r\n\r\nabcdef", r));
    CHECK(r.status == 200 && r.body == "ab");
    // No Content-Length: the rest
    CHECK(parseHttpResponse("HTTP/1.1 401 Unauthorized\r\nX: y\r\n\r\n{}", r));
    CHECK(r.status == 401 && r.body == "{}");
    CHECK(parseHttpResponse("HTTP/1.1 204 No Content\r\n\r\n", r));
    CHECK(r.status == 204 && r.body.empty());
    // Cut short, or not HTTP
    CHECK(!parseHttpResponse("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc", r));
    CHECK(!parseHttpResponse("HTTP/1.1 200 OK\r\nContent-Length: x\r\n\r\nabc", r));
    CHECK(!parseHttpResponse("HTTP/1.1 200 OK\r\nContent-Length: 2", r));
    CHECK(!parseHttpResponse("HTTP/2 200 OK\r\n\r\n", r));
    CHECK(!parseHttpResponse("HTTP/1.1 2x0 OK\r\n\r\n", r));
    CHECK(!parseHttpResponse("", r));
    CHECK(!parseHttpResponse("SSH-2.0-OpenSSH\r\n\r\n", r));
}

void test_levels()
{
    CHECK(levelFor(-1) == Level::Unknown);
    CHECK(levelFor(0) == Level::Normal);
    CHECK(levelFor(kWarnPct - 1) == Level::Normal);
    CHECK(levelFor(kWarnPct) == Level::Warn);
    CHECK(levelFor(kCriticalPct - 1) == Level::Warn);
    CHECK(levelFor(kCriticalPct) == Level::Critical);
    CHECK(levelFor(100) == Level::Critical);

    // A window past its reset starts over
    Window w{95, kNow - 1};
    CHECK(pctNow(w, kNow) == 0);
    w.resetsAt = kNow + 60;
    CHECK(pctNow(w, kNow) == 95);
    w.resetsAt = 0;
    CHECK(pctNow(w, kNow) == 95);
    w.pct = -1;
    w.resetsAt = kNow - 1;
    CHECK(pctNow(w, kNow) == -1);

    Snapshot s;
    CHECK(worstLevel(s, kNow) == Level::Unknown);
    s.session = {2, kNow + 3600};
    s.weekly  = {86, kNow + 86400};
    CHECK(worstLevel(s, kNow) == Level::Warn);
    s.session.pct = 93;
    CHECK(worstLevel(s, kNow) == Level::Critical);
    s.session.resetsAt = kNow - 10;  // that one started over
    CHECK(worstLevel(s, kNow) == Level::Warn);
}

void test_palette()
{
    CHECK(!isNight(7) && !isNight(12) && !isNight(21));
    CHECK(isNight(22) && isNight(23) && isNight(0) && isNight(6));
    // The backlight while the screensaver shows: on a cable the user's, day and night (no power saving); on the
    // battery never above the user's, lower at night
    for (const bool night : {false, true}) {
        CHECK(screensaverBrightness(80, night, true) == 80);
        CHECK(screensaverBrightness(5, night, true) == 5);
        CHECK(screensaverBrightness(100, night, true) == 100);
    }
    CHECK(screensaverBrightness(80, false, false) == kDayBrightness);
    CHECK(screensaverBrightness(80, true, false) == kNightBrightness);
    CHECK(screensaverBrightness(20, false, false) == 20);
    CHECK(screensaverBrightness(5, true, false) == 5);
    CHECK(screensaverBrightness(0, false, false) == 0);
    CHECK(kNightBrightness < kDayBrightness);

    for (int i = 0; i < kStyleCount; ++i) {
        const Palette& p = paletteFor(static_cast<Style>(i));
        CHECK(levelColor(p, Level::Normal) == p.normal);
        CHECK(levelColor(p, Level::Warn) == p.warn);
        CHECK(levelColor(p, Level::Critical) == p.critical);
        CHECK(levelColor(p, Level::Unknown) == p.muted);
        CHECK(stateColor(p, AgentState::Work) == p.work);
        CHECK(stateColor(p, AgentState::Wait) == p.wait);
        CHECK(stateColor(p, AgentState::Done) == p.done);
        CHECK(stateColor(p, AgentState::Idle) == p.idle);
        CHECK(stateColor(p, AgentState::Unknown) == p.idle);
        // Dark: the background true black (any grey glows on the LCD), everything on it lighter
        const auto luma = [](uint32_t c) {
            return ((c >> 16 & 0xFF) * 299 + (c >> 8 & 0xFF) * 587 + (c & 0xFF) * 114) / 1000;
        };
        CHECK(p.bg == 0x000000);
        for (const uint32_t c : {p.text, p.muted, p.dim, p.track, p.normal, p.warn, p.critical, p.work, p.wait,
                                 p.done, p.idle}) {
            CHECK(luma(c) > luma(p.bg));
        }
        CHECK(luma(p.text) > luma(p.muted) && luma(p.muted) > luma(p.dim));
        CHECK(p.warn != p.normal && p.critical != p.warn && p.critical != p.normal);
    }
    // Every style its own accent (the background is black in all)
    for (int a = 0; a < kStyleCount; ++a) {
        for (int b = a + 1; b < kStyleCount; ++b) {
            const Palette& pa = paletteFor(static_cast<Style>(a));
            const Palette& pb = paletteFor(static_cast<Style>(b));
            CHECK(pa.normal != pb.normal);
        }
    }
}

void test_styles()
{
    const char* keys[] = {"claude", "rings", "terminal", "sleepy"};
    for (int i = 0; i < kStyleCount; ++i) {
        const Style s = static_cast<Style>(i);
        CHECK(std::string(styleKey(s)) == keys[i]);
        CHECK(std::string(styleName(s)).size() > 0);
        Style parsed = Style::Claude;
        CHECK(parseStyle(keys[i], parsed) && parsed == s);
        CHECK(nextStyle(nextStyle(s, 1), -1) == s);
    }
    Style kept = Style::Rings;
    CHECK(!parseStyle("", kept) && !parseStyle("Claude", kept) && !parseStyle("ring", kept) && kept == Style::Rings);
    // Round the list both ways
    CHECK(nextStyle(Style::Claude, 1) == Style::Rings);
    CHECK(nextStyle(Style::Sleepy, 1) == Style::Claude);
    CHECK(nextStyle(Style::Claude, -1) == Style::Sleepy);
    Style s = Style::Terminal;
    for (int i = 0; i < kStyleCount; ++i) {
        s = nextStyle(s, 1);
    }
    CHECK(s == Style::Terminal);
}

void test_texts()
{
    CHECK(clockText(at(2026, 10, 13, 19, 55, 2)) == "19:55");
    CHECK(clockText(at(2026, 1, 2, 9, 5, 5)) == "09:05");
    CHECK(clockText(at(2026, 1, 2, 0, 0, 5)) == "00:00");
    CHECK(clockText(at(1970, 1, 1, 8, 0, 4)) == "--:--");  // not set yet
    CHECK(!clockValid(at(2024, 12, 31, 23, 59, 2)) && clockValid(at(2025, 1, 1, 0, 0, 3)));
    CHECK(dateText(at(2026, 10, 13, 19, 55, 2)) == "10月13日 周二");
    CHECK(dateText(at(2026, 3, 1, 8, 0, 0)) == "3月1日 周日");
    CHECK(dateText(at(2026, 3, 7, 8, 0, 6)) == "3月7日 周六");
    CHECK(dateText(at(1970, 1, 1, 8, 0, 4)).empty());
    CHECK(dateEnglish(at(2026, 10, 13, 19, 55, 2)) == "TUE 13 OCT");
    CHECK(dateEnglish(at(2026, 3, 1, 8, 0, 0)) == "SUN 01 MAR");
    CHECK(dateEnglish(at(1970, 1, 1, 8, 0, 4)).empty());

    CHECK(stateText(AgentState::Work, 0) == "工作中");
    CHECK(stateText(AgentState::Wait, 0) == "等你确认");
    CHECK(stateText(AgentState::Wait, 1) == "等你确认");
    CHECK(stateText(AgentState::Wait, 3) == "3 个等你确认");
    CHECK(stateText(AgentState::Done, 0) == "已完成");
    CHECK(stateText(AgentState::Idle, 0) == "空闲");
    CHECK(stateText(AgentState::Unknown, 0).empty());
    CHECK(stateEnglish(AgentState::Work, 0) == "WORKING");
    CHECK(stateEnglish(AgentState::Wait, 1) == "AWAITING INPUT");
    CHECK(stateEnglish(AgentState::Wait, 2) == "AWAITING INPUT x2");
    CHECK(stateEnglish(AgentState::Done, 0) == "DONE");
    CHECK(stateEnglish(AgentState::Idle, 0) == "IDLE");
    CHECK(stateEnglish(AgentState::Unknown, 0).empty());

    // The terminal's bars and columns
    CHECK(barText(-1) == "[----------]");
    CHECK(barText(0) == "[----------]");
    CHECK(barText(4) == "[----------]");
    CHECK(barText(5) == "[#---------]");
    CHECK(barText(23) == "[##--------]");
    CHECK(barText(86) == "[#########-]");
    CHECK(barText(100) == "[##########]");
    CHECK(barText(130) == "[##########]");
    CHECK(pctColumn(9) == "  9%" && pctColumn(87) == " 87%" && pctColumn(100) == "100%" && pctColumn(-1) == " --%");
    CHECK(pctColumn(140) == "100%");
}

void test_reset_text()
{
    // Local time is UTC+8 here (the Mac's and the robot's)
    setenv("TZ", "CST-8", 1);
    tzset();
    CHECK(resetText(0, kNow).empty());
    CHECK(resetText(kNow - 100, kNow) == "已重置");
    CHECK(resetText(kNow - 20, kNow) == "已重置");  // passed, though it would round to 19:56
    CHECK(resetText(kNow, kNow) == "已重置");
    CHECK(resetText(kNow + 10, kNow) == "马上重置");  // rounds back to 19:56:00
    CHECK(resetText(kNow + 30, kNow) == "马上重置");
    CHECK(resetText(kNow + 64, kNow) == "1 分后重置");     // 19:57:00
    CHECK(resetText(kNow + 3540, kNow) == "59 分后重置");  // 20:54:56 -> 20:55
    CHECK(resetText(kNow + 3600, kNow) == "1:00 后重置");  // 20:56
    // The session window of the snapshot: Wed 00:39:59 -> 00:40, 4:44 from 19:55:56
    CHECK(resetText(1791909599, kNow) == "4:44 后重置");
    CHECK(resetText(kNow + 86340, kNow) == "23:59 后重置");
    // A day or more: the weekday (the weekly window: Thu 14:59:59 -> 15:00)
    CHECK(resetText(1792047599, kNow) == "周四 15:00 重置");
    CHECK(resetText(kNow + 2 * 86400 + 3600, kNow) == "周四 20:56 重置");
    // Six days or more: the date
    CHECK(resetText(kNow + 7 * 86400, kNow) == "10月20日 重置");

    // Short (a narrow column) and English (the terminal)
    CHECK(resetShort(0, kNow).empty() && resetEnglish(0, kNow).empty());
    CHECK(resetShort(kNow - 1, kNow) == "已重置" && resetEnglish(kNow - 1, kNow) == "RESET");
    CHECK(resetShort(kNow + 10, kNow) == "马上" && resetEnglish(kNow + 10, kNow) == "NOW");
    CHECK(resetShort(kNow + 3540, kNow) == "59 分后" && resetEnglish(kNow + 3540, kNow) == "IN 0:59");
    CHECK(resetShort(1791909599, kNow) == "4:44 后" && resetEnglish(1791909599, kNow) == "IN 4:44");
    CHECK(resetShort(1792047599, kNow) == "周四 15:00" && resetEnglish(1792047599, kNow) == "THU 15:00");
    CHECK(resetShort(kNow + 7 * 86400, kNow) == "10月20日" && resetEnglish(kNow + 7 * 86400, kNow) == "OCT 20");
}

void test_feed_note()
{
    // Recent usage: nothing to say, whatever the last fetch did
    for (const FeedStatus st : {FeedStatus::Ok, FeedStatus::Waiting, FeedStatus::NoRouter, FeedStatus::Unreachable,
                                FeedStatus::Unauthorized, FeedStatus::BadReply}) {
        CHECK(feedNote(st, true, 0).empty());
        CHECK(feedNote(st, true, kStaleMs).empty());
    }
    // Nothing yet
    CHECK(feedNote(FeedStatus::Waiting, false, 0) == "正在取用量...");
    CHECK(feedNote(FeedStatus::Unreachable, false, 0) == "连不上 Mac");
    CHECK(feedNote(FeedStatus::NoRouter, false, 0) == "连不上 Wi-Fi");
    CHECK(feedNote(FeedStatus::Unauthorized, false, 0) == "Mac 拒绝了令牌");
    CHECK(feedNote(FeedStatus::BadReply, false, 0) == "Mac 回的不是用量");
    // Not set up, no WiFi: said even over old usage
    CHECK(feedNote(FeedStatus::NotSetUp, false, 0) == "用量未配置");
    CHECK(feedNote(FeedStatus::NotSetUp, true, kStaleMs + 1) == "用量未配置");
    CHECK(feedNote(FeedStatus::NoWifi, false, 0) == "机器人还没配 Wi-Fi");
    // Old usage: why, and how old
    CHECK(feedNote(FeedStatus::Unreachable, true, 12 * 60000) == "连不上 Mac · 12 分钟前");
    CHECK(feedNote(FeedStatus::Unreachable, true, 119 * 60000) == "连不上 Mac · 119 分钟前");
    CHECK(feedNote(FeedStatus::Unreachable, true, 5 * 3600000) == "连不上 Mac · 5 小时前");
    CHECK(feedNote(FeedStatus::Waiting, true, 6 * 60000) == "正在更新 · 6 分钟前");

    // Only the reason (a narrow place), and the terminal's
    CHECK(feedReason(FeedStatus::Unreachable, true, 0).empty());
    CHECK(feedReason(FeedStatus::Unreachable, true, 12 * 60000) == "连不上 Mac");
    CHECK(feedReason(FeedStatus::NotSetUp, false, 0) == "用量未配置");
    CHECK(feedReason(FeedStatus::Waiting, false, 0) == "正在取用量...");
    CHECK(feedNoteEnglish(FeedStatus::Unreachable, true, 0).empty());
    CHECK(feedNoteEnglish(FeedStatus::Unreachable, true, 12 * 60000) == "MAC OFFLINE 12M AGO");
    CHECK(feedNoteEnglish(FeedStatus::Unreachable, true, 5 * 3600000) == "MAC OFFLINE 5H AGO");
    CHECK(feedNoteEnglish(FeedStatus::NotSetUp, true, 9 * 60000) == "NOT SET UP");
    CHECK(feedNoteEnglish(FeedStatus::Waiting, false, 0) == "FETCHING...");
    CHECK(feedNoteEnglish(FeedStatus::Unauthorized, false, 0) == "TOKEN REFUSED");
}

void test_float()
{
    // Within the amplitudes, a pixel a quarter second at most, and over a few minutes all over its area
    for (const int amp : {0, 3, 14, 40}) {
        int px = 0;
        int py = 0;
        floatOffset(0, amp, amp, px, py);
        int min_x = px, max_x = px, min_y = py, max_y = py;
        for (uint32_t ms = 250; ms < 2 * kFloatPeriodXMs; ms += 250) {
            int dx = 99;
            int dy = 99;
            floatOffset(ms, amp, amp, dx, dy);
            CHECK(dx >= -amp && dx <= amp && dy >= -amp && dy <= amp);
            CHECK(std::abs(dx - px) <= 1 && std::abs(dy - py) <= 1);
            min_x = std::min(min_x, dx);
            max_x = std::max(max_x, dx);
            min_y = std::min(min_y, dy);
            max_y = std::max(max_y, dy);
            px = dx;
            py = dy;
        }
        CHECK(min_x == -amp && max_x == amp && min_y == -amp && max_y == amp);
    }
    // The same moment, the same place; the two directions out of step
    int ax = 0, ay = 0, bx = 0, by = 0;
    floatOffset(123456, 14, 8, ax, ay);
    floatOffset(123456, 14, 8, bx, by);
    CHECK(ax == bx && ay == by);
    CHECK(kFloatPeriodXMs != kFloatPeriodYMs);
    // Across the uptime wrap (49 days): no jump of more than the step
    int wx = 0, wy = 0, vx = 0, vy = 0;
    floatOffset(0xFFFFFF00u, 14, 8, wx, wy);
    floatOffset(0xFFFFFF00u + 250u, 14, 8, vx, vy);
    CHECK(std::abs(vx - wx) <= 14 && std::abs(vy - wy) <= 8);
}

void test_config()
{
    Config c;
    CHECK(parseCommand("stackpet usage 192.168.31.90 8787 abcdefghijklmnopqrstuvwx", c) == Command::Set);
    CHECK(c.host == "192.168.31.90" && c.port == 8787 && c.token == "abcdefghijklmnopqrstuvwx");
    CHECK(c.valid());
    CHECK(parseCommand("  stackpet   usage mac-mini.lan 1 A-b_c.d~e+f/g=h \r", c) == Command::Set);
    CHECK(c.host == "mac-mini.lan" && c.port == 1 && c.token == "A-b_c.d~e+f/g=h");
    CHECK(parseCommand("stackpet usage clear", c) == Command::Clear);
    CHECK(parseCommand("stackpet usage show\r", c) == Command::Show);
    std::string value;
    CHECK(parseCommand("stackpet usage style rings", c, &value) == Command::Style && value == "rings");
    CHECK(parseCommand("stackpet usage style sleepy", c) == Command::Style);  // value not wanted
    CHECK(parseCommand("stackpet usage tz CST-8", c, &value) == Command::Timezone && value == "CST-8");
    CHECK(parseCommand("stackpet usage tz CET-1CEST,M3.5.0,M10.5.0/3", c, &value) == Command::Timezone);
    CHECK(parseCommand("stackpet usage tz <+0545>-5:45", c, &value) == Command::Timezone && value == "<+0545>-5:45");
    CHECK(validTimezone("GMT0") && !validTimezone("UT") && !validTimezone("CST-8;rm") && !validTimezone(std::string(64, 'A')));

    // Bad lines leave out alone
    Config kept;
    kept.host = "kept";
    for (const char* bad : {"stackpet usage", "stackpet usage 10.0.0.1 8787", "stackpet usage 10.0.0.1 0 abcdefgh",
                            "stackpet usage 10.0.0.1 65536 abcdefgh", "stackpet usage 10.0.0.1 80x abcdefgh",
                            "stackpet usage 10.0.0.1 123456 abcdefgh", "stackpet usage 10.0.0.1 8787 short",
                            "stackpet usage 10.0.0.1 8787 bad\"token1", "stackpet usage 10.0.0.1 8787 abcdefgh extra",
                            "stackpet usage bad_host 8787 abcdefgh", "stackpet usage .lead 8787 abcdefgh",
                            "stackpet usage -lead 8787 abcdefgh", "stackpet usage clear now",
                            "stackpet usage style", "stackpet usage style plain", "stackpet usage style rings now",
                            "stackpet usage tz", "stackpet usage tz C$T", "stackpet usage tz CST -8"}) {
        CHECK(parseCommand(bad, kept) == Command::Bad);
    }
    CHECK(kept.host == "kept");
    for (const char* other : {"", "hello", "stackpet", "stackpet face", "usage 1 2 3", "STACKPET usage clear"}) {
        CHECK(parseCommand(other, kept) == Command::None);
    }

    CHECK(validHost("a") && validHost("192.168.1.2") && validHost(std::string(kHostMax, 'a')));
    CHECK(!validHost("") && !validHost(std::string(kHostMax + 1, 'a')) && !validHost("a b") && !validHost("a:1"));
    CHECK(validToken(std::string(kTokenMin, 'x')) && validToken(std::string(kTokenMax, 'x')));
    CHECK(!validToken(std::string(kTokenMin - 1, 'x')) && !validToken(std::string(kTokenMax + 1, 'x')));
    CHECK(!validToken("abcdefg h") && !validToken("abcdefg\x01"));
    Config empty;
    CHECK(!empty.valid());
}

}  // namespace

int main()
{
    RUN_TEST(test_snapshot);
    RUN_TEST(test_http);
    RUN_TEST(test_levels);
    RUN_TEST(test_palette);
    RUN_TEST(test_styles);
    RUN_TEST(test_texts);
    RUN_TEST(test_reset_text);
    RUN_TEST(test_feed_note);
    RUN_TEST(test_float);
    RUN_TEST(test_config);
    std::cout << "all usage tests passed (" << stackpet_test::checks() << " checks)\n";
    return 0;
}
