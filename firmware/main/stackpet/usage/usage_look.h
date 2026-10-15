/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
// StackPet: how the launcher's screensaver (the clock and Claude's usage) looks: its styles and their colors, the
// colors for the usage and what Claude is doing, the texts, how the picture floats. Pure, host tested: texts in
// Chinese (the 20 px CN font) or English (the terminal style), colors as 0xRRGGBB
#pragma once
#include "usage_snapshot.h"
#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>

namespace stackpet::usage {

// The screensaver's styles, all dark (a screensaver: little light, never still)
enum class Style {
    Claude,    // 暖黑: Claude's warm black and terracotta, the two windows side by side
    Rings,     // 表盘: the two windows as rings round the clock, a dot going round each minute
    Terminal,  // 终端: green phosphor, scan lines, a blinking cursor
    Sleepy,    // 睡觉: StackChan asleep under the clock (eyes open when Claude waits for the user)
};
constexpr int kStyleCount = 4;
const char* styleKey(Style s);   // "claude", "rings", "terminal", "sleepy"
const char* styleName(Style s);  // "暖黑", "表盘", "终端", "睡觉"
bool parseStyle(std::string_view key, Style& out);
Style nextStyle(Style s, int step);  // step ±1, round the list

// How close a window is to its limit
enum class Level { Unknown, Normal, Warn, Critical };
constexpr int kWarnPct     = 70;
constexpr int kCriticalPct = 90;
Level levelFor(int pct);
// A window whose reset time has passed starts over at 0% (the Mac's number is from before)
int pctNow(const Window& w, int64_t now);
// The window closest to its limit
Level worstLevel(const Snapshot& s, int64_t now);

// Night, 22:00 to 06:59 local: nothing blinks (and on the battery the backlight goes down further)
constexpr int kNightFromHour = 22;
constexpr int kNightToHour   = 7;
bool isNight(int hour);
// The backlight while the screensaver shows (0..100): on a cable what the user set (no power saving there); on the
// battery turned down, more at night, never above what the user set
constexpr uint8_t kDayBrightness   = 30;
constexpr uint8_t kNightBrightness = 8;
uint8_t screensaverBrightness(uint8_t user, bool night, bool externalPower);

struct Palette {
    uint32_t bg, text, muted, dim, track;
    uint32_t normal, warn, critical;  // the usage by level (the style's accent while normal)
    uint32_t work, wait, done, idle;  // Claude's state
};
const Palette& paletteFor(Style s);
uint32_t levelColor(const Palette& p, Level level);
uint32_t stateColor(const Palette& p, AgentState state);

// "09:05" (24 hours); "--:--" while the clock is not set (a year before 2025)
bool clockValid(const std::tm& local);
std::string clockText(const std::tm& local);
// "10月13日 周二" / "TUE 13 OCT"; "" while the clock is not set
std::string dateText(const std::tm& local);
std::string dateEnglish(const std::tm& local);

// When the window starts over (to the nearest minute). Long: "马上重置", "37 分后重置", "4:12 后重置" (within a day),
// "周四 15:00 重置" (local, within six days), "10月20日 重置", "已重置" (passed). Short (a narrow column): "马上",
// "37 分后", "4:12 后", "周四 15:00", "10月20日", "已重置". English: "NOW", "IN 0:37", "IN 4:12", "THU 15:00",
// "OCT 20", "RESET". "" if not known
std::string resetText(int64_t resetsAt, int64_t now);
std::string resetShort(int64_t resetsAt, int64_t now);
std::string resetEnglish(int64_t resetsAt, int64_t now);

// Claude's state: "工作中", "等你确认" ("2 个等你确认"), "已完成", "空闲" / "WORKING", "AWAITING INPUT"
// ("AWAITING INPUT x2"), "DONE", "IDLE"; "" if not known
std::string stateText(AgentState state, int waiting);
std::string stateEnglish(AgentState state, int waiting);

// "[######----]": ten cells, a cell for each 10% (rounded); all '-' if not known
std::string barText(int pct);
// "  9%", " 87%", "100%", " --%": four wide (the terminal's columns line up)
std::string pctColumn(int pct);

// How the usage feed stands
enum class FeedStatus {
    NotSetUp,      // no Mac address or token in NVS (tools/stackpet_usage_setup.py sets them)
    NoWifi,        // the robot has no WiFi saved (SETUP)
    Waiting,       // the first fetch still going
    Ok,            // the last fetch worked
    NoRouter,      // the WiFi did not connect
    Unreachable,   // the Mac did not answer (asleep, or ClaudeUsageBar not running)
    Unauthorized,  // the Mac refused the token (made again in its pairing window)
    BadReply,      // an answer that is not the snapshot
};
// Older than this the usage is shown dimmed, with its age
constexpr uint32_t kStaleMs = 5 * 60 * 1000;
// Why there is no fresh usage, "" when it is fresh. ageMs: since the shown snapshot came.
// "连不上 Mac · 12 分钟前" / "MAC OFFLINE 12M AGO"
std::string feedNote(FeedStatus status, bool haveSnapshot, uint32_t ageMs);
std::string feedNoteEnglish(FeedStatus status, bool haveSnapshot, uint32_t ageMs);
// Only why, without the age ("连不上 Mac"), for a narrow place; "" when the usage is fresh
std::string feedReason(FeedStatus status, bool haveSnapshot, uint32_t ageMs);

// The picture floats slowly (the same pixels lit all day would mark the screen, and a screensaver should look alive):
// an offset within ±ampX, ±ampY at uptime ms, two slow sines that never line up. At most a pixel each quarter second
// for amplitudes up to 40
void floatOffset(uint32_t ms, int ampX, int ampY, int& dx, int& dy);
constexpr uint32_t kFloatPeriodXMs = 173000;
constexpr uint32_t kFloatPeriodYMs = 131000;

}  // namespace stackpet::usage
