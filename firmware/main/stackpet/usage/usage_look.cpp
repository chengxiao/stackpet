/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "usage_look.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace stackpet::usage {

namespace {

const char* const kWeekdays[]   = {"日", "一", "二", "三", "四", "五", "六"};
const char* const kWeekdaysEn[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
const char* const kMonthsEn[]   = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

// The background true black in every style: the LCD shows a dark grey (0x141312 was) as a glow, lit by the backlight
// over the whole screen; the style is in the colours on it
constexpr Palette kPalettes[kStyleCount] = {
    // 暖黑: cream digits, terracotta (Claude's colours)
    {0x000000, 0xEDE5D3, 0x8A8377, 0x4A4640, 0x2A2724,  // bg, text, muted, dim, track
     0xD97757, 0xE8A33D, 0xE5534B,                      // normal, warn, critical
     0xD97757, 0xE8A33D, 0x6FBF8B, 0x6B665E},           // work, wait, done, idle
    // 表盘: white, sky blue
    {0x000000, 0xF2F0EA, 0x8E8C86, 0x3C3B38, 0x1E1E1E,  //
     0x58C4DD, 0xF2B544, 0xF0564A,                      //
     0x58C4DD, 0xF2B544, 0x8BD17C, 0x6E6E6E},           //
    // 终端: green phosphor, amber and red for the warnings
    {0x000000, 0x46F07A, 0x2A9C4E, 0x145C2B, 0x0E2E17,  //
     0x46F07A, 0xFFB52E, 0xFF4D3D,                      //
     0x46F07A, 0xFFB52E, 0x46F07A, 0x2A9C4E},           //
    // 睡觉: soft lavender
    {0x000000, 0xE6E0F0, 0x8C86A0, 0x4A4658, 0x24222C,  //
     0xB59CFF, 0xF2B544, 0xF0564A,                      //
     0xB59CFF, 0xF2B544, 0x7FD8A8, 0x6E6880},           //
};

const char* const kStyleKeys[kStyleCount]  = {"claude", "rings", "terminal", "sleepy"};
const char* const kStyleNames[kStyleCount] = {"暖黑", "表盘", "终端", "睡觉"};

std::string format(const char* fmt, int a, int b = 0)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), fmt, a, b);
    return buf;
}

std::tm local_of(int64_t t)
{
    const std::time_t tt = static_cast<std::time_t>(t);
    std::tm local{};
    localtime_r(&tt, &local);
    return local;
}

// When it resets, sorted out once for the three wordings: -2 not known, -1 passed, else the seconds left to the
// reset time rounded to the nearest minute (the Mac says a window ends at 14:59:59, that is 15:00)
int64_t left_of(int64_t resetsAt, int64_t now, int64_t& at)
{
    if (resetsAt <= 0) {
        return -2;
    }
    if (resetsAt <= now) {
        return -1;  // as pctNow(): from the exact time
    }
    at = (resetsAt + 30) / 60 * 60;
    return std::max<int64_t>(at - now, 0);
}

}  // namespace

const char* styleKey(Style s)
{
    return kStyleKeys[static_cast<int>(s)];
}

const char* styleName(Style s)
{
    return kStyleNames[static_cast<int>(s)];
}

bool parseStyle(std::string_view key, Style& out)
{
    for (int i = 0; i < kStyleCount; ++i) {
        if (key == kStyleKeys[i]) {
            out = static_cast<Style>(i);
            return true;
        }
    }
    return false;
}

Style nextStyle(Style s, int step)
{
    const int n = ((static_cast<int>(s) + step) % kStyleCount + kStyleCount) % kStyleCount;
    return static_cast<Style>(n);
}

Level levelFor(int pct)
{
    if (pct < 0) {
        return Level::Unknown;
    }
    if (pct >= kCriticalPct) {
        return Level::Critical;
    }
    return pct >= kWarnPct ? Level::Warn : Level::Normal;
}

int pctNow(const Window& w, int64_t now)
{
    if (w.pct >= 0 && w.resetsAt > 0 && w.resetsAt <= now) {
        return 0;
    }
    return w.pct;
}

Level worstLevel(const Snapshot& s, int64_t now)
{
    return std::max(levelFor(pctNow(s.session, now)), levelFor(pctNow(s.weekly, now)));
}

bool isNight(int hour)
{
    return hour >= kNightFromHour || hour < kNightToHour;
}

uint8_t screensaverBrightness(uint8_t user, bool night, bool externalPower)
{
    if (externalPower) {
        return user;
    }
    return std::min(user, night ? kNightBrightness : kDayBrightness);
}

const Palette& paletteFor(Style s)
{
    return kPalettes[static_cast<int>(s)];
}

uint32_t levelColor(const Palette& p, Level level)
{
    switch (level) {
        case Level::Critical:
            return p.critical;
        case Level::Warn:
            return p.warn;
        case Level::Normal:
            return p.normal;
        case Level::Unknown:
            break;
    }
    return p.muted;
}

uint32_t stateColor(const Palette& p, AgentState state)
{
    switch (state) {
        case AgentState::Work:
            return p.work;
        case AgentState::Wait:
            return p.wait;
        case AgentState::Done:
            return p.done;
        case AgentState::Idle:
        case AgentState::Unknown:
            break;
    }
    return p.idle;
}

bool clockValid(const std::tm& local)
{
    return local.tm_year + 1900 >= 2025;
}

std::string clockText(const std::tm& local)
{
    if (!clockValid(local)) {
        return "--:--";
    }
    return format("%02d:%02d", local.tm_hour, local.tm_min);
}

std::string dateText(const std::tm& local)
{
    if (!clockValid(local)) {
        return "";
    }
    return format("%d月%d日 周", local.tm_mon + 1, local.tm_mday) + kWeekdays[std::clamp(local.tm_wday, 0, 6)];
}

std::string dateEnglish(const std::tm& local)
{
    if (!clockValid(local)) {
        return "";
    }
    return std::string(kWeekdaysEn[std::clamp(local.tm_wday, 0, 6)]) + format(" %02d ", local.tm_mday) +
           kMonthsEn[std::clamp(local.tm_mon, 0, 11)];
}

std::string resetText(int64_t resetsAt, int64_t now)
{
    int64_t at         = 0;
    const int64_t left = left_of(resetsAt, now, at);
    if (left == -2) {
        return "";
    }
    if (left == -1) {
        return "已重置";
    }
    if (left < 60) {
        return "马上重置";
    }
    if (left < 6 * 86400 && left >= 86400) {
        const std::tm t = local_of(at);
        return std::string("周") + kWeekdays[std::clamp(t.tm_wday, 0, 6)] + format(" %02d:%02d 重置", t.tm_hour, t.tm_min);
    }
    if (left >= 86400) {
        const std::tm t = local_of(at);
        return format("%d月%d日 重置", t.tm_mon + 1, t.tm_mday);  // no time: the line has no room for it
    }
    return resetShort(resetsAt, now) + "重置";
}

std::string resetShort(int64_t resetsAt, int64_t now)
{
    int64_t at         = 0;
    const int64_t left = left_of(resetsAt, now, at);
    if (left == -2) {
        return "";
    }
    if (left == -1) {
        return "已重置";
    }
    if (left < 60) {
        return "马上";
    }
    if (left < 3600) {
        return format("%d 分后", static_cast<int>(left / 60));
    }
    if (left < 86400) {
        return format("%d:%02d 后", static_cast<int>(left / 3600), static_cast<int>(left % 3600 / 60));
    }
    const std::tm t = local_of(at);
    if (left < 6 * 86400) {
        return std::string("周") + kWeekdays[std::clamp(t.tm_wday, 0, 6)] + format(" %02d:%02d", t.tm_hour, t.tm_min);
    }
    return format("%d月%d日", t.tm_mon + 1, t.tm_mday);
}

std::string resetEnglish(int64_t resetsAt, int64_t now)
{
    int64_t at         = 0;
    const int64_t left = left_of(resetsAt, now, at);
    if (left == -2) {
        return "";
    }
    if (left == -1) {
        return "RESET";
    }
    if (left < 60) {
        return "NOW";
    }
    if (left < 86400) {
        return format("IN %d:%02d", static_cast<int>(left / 3600), static_cast<int>(left % 3600 / 60));
    }
    const std::tm t = local_of(at);
    if (left < 6 * 86400) {
        return std::string(kWeekdaysEn[std::clamp(t.tm_wday, 0, 6)]) + format(" %02d:%02d", t.tm_hour, t.tm_min);
    }
    return std::string(kMonthsEn[std::clamp(t.tm_mon, 0, 11)]) + format(" %d", t.tm_mday);
}

std::string stateText(AgentState state, int waiting)
{
    switch (state) {
        case AgentState::Work:
            return "工作中";
        case AgentState::Wait:
            return waiting > 1 ? format("%d 个等你确认", waiting) : "等你确认";
        case AgentState::Done:
            return "已完成";
        case AgentState::Idle:
            return "空闲";
        case AgentState::Unknown:
            break;
    }
    return "";
}

std::string stateEnglish(AgentState state, int waiting)
{
    switch (state) {
        case AgentState::Work:
            return "WORKING";
        case AgentState::Wait:
            return waiting > 1 ? format("AWAITING INPUT x%d", waiting) : "AWAITING INPUT";
        case AgentState::Done:
            return "DONE";
        case AgentState::Idle:
            return "IDLE";
        case AgentState::Unknown:
            break;
    }
    return "";
}

std::string barText(int pct)
{
    const int cells = pct < 0 ? 0 : std::clamp((pct + 5) / 10, 0, 10);
    return "[" + std::string(cells, '#') + std::string(10 - cells, '-') + "]";
}

std::string pctColumn(int pct)
{
    if (pct < 0) {
        return " --%";
    }
    return format("%3d%%", std::min(pct, 100));
}

std::string feedReason(FeedStatus status, bool haveSnapshot, uint32_t ageMs)
{
    // A fetch gone wrong once says nothing while what is shown is still recent
    if (haveSnapshot && ageMs <= kStaleMs) {
        return "";
    }
    switch (status) {
        case FeedStatus::NotSetUp:
            return "用量未配置";
        case FeedStatus::NoWifi:
            return "机器人还没配 Wi-Fi";
        case FeedStatus::Waiting:
            return haveSnapshot ? "正在更新" : "正在取用量...";
        case FeedStatus::Ok:
            return "数据没有更新";
        case FeedStatus::NoRouter:
            return "连不上 Wi-Fi";
        case FeedStatus::Unreachable:
            return "连不上 Mac";
        case FeedStatus::Unauthorized:
            return "Mac 拒绝了令牌";
        case FeedStatus::BadReply:
            return "Mac 回的不是用量";
    }
    return "";
}

std::string feedNote(FeedStatus status, bool haveSnapshot, uint32_t ageMs)
{
    const std::string reason = feedReason(status, haveSnapshot, ageMs);
    // Not set up, no WiFi, nothing yet: the reason is all
    if (reason.empty() || !haveSnapshot || status == FeedStatus::NotSetUp || status == FeedStatus::NoWifi) {
        return reason;
    }
    const uint32_t minutes = ageMs / 60000;
    return minutes < 120 ? reason + format(" · %d 分钟前", static_cast<int>(std::max<uint32_t>(minutes, 1)))
                         : reason + format(" · %d 小时前", static_cast<int>(minutes / 60));
}

std::string feedNoteEnglish(FeedStatus status, bool haveSnapshot, uint32_t ageMs)
{
    if (haveSnapshot && ageMs <= kStaleMs) {
        return "";
    }
    std::string reason;
    switch (status) {
        case FeedStatus::NotSetUp:
            return "NOT SET UP";
        case FeedStatus::NoWifi:
            return "NO WIFI SAVED";
        case FeedStatus::Waiting:
            if (!haveSnapshot) {
                return "FETCHING...";
            }
            reason = "UPDATING";
            break;
        case FeedStatus::Ok:
            reason = "NO UPDATE";
            break;
        case FeedStatus::NoRouter:
            reason = "WIFI DOWN";
            break;
        case FeedStatus::Unreachable:
            reason = "MAC OFFLINE";
            break;
        case FeedStatus::Unauthorized:
            reason = "TOKEN REFUSED";
            break;
        case FeedStatus::BadReply:
            reason = "BAD REPLY";
            break;
    }
    if (!haveSnapshot) {
        return reason;
    }
    const uint32_t minutes = ageMs / 60000;
    return minutes < 120 ? reason + format(" %dM AGO", static_cast<int>(std::max<uint32_t>(minutes, 1)))
                         : reason + format(" %dH AGO", static_cast<int>(minutes / 60));
}

void floatOffset(uint32_t ms, int ampX, int ampY, int& dx, int& dy)
{
    constexpr double kTwoPi = 6.283185307179586;
    const double x = std::sin(kTwoPi * static_cast<double>(ms % kFloatPeriodXMs) / kFloatPeriodXMs);
    const double y = std::sin(kTwoPi * static_cast<double>(ms % kFloatPeriodYMs) / kFloatPeriodYMs + 1.0);
    dx             = static_cast<int>(std::lround(x * ampX));
    dy             = static_cast<int>(std::lround(y * ampY));
}

}  // namespace stackpet::usage
