/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
// StackPet: where the Claude usage comes from (the Mac's address, port and token), set over USB by
// tools/stackpet_usage_setup.py: a line on the USB-Serial/JTAG port, kept in NVS. Pure: the line and the checks, host
// tested (the NVS part is usage_feed.cpp)
#pragma once
#include <string>
#include <string_view>

namespace stackpet::usage {

constexpr int kDefaultPort      = 8787;  // ClaudeUsageBar's LAN service
constexpr size_t kHostMax       = 63;
constexpr size_t kTokenMin      = 8;
constexpr size_t kTokenMax      = 128;
constexpr size_t kConsoleLineMax = 200;  // longer lines are dropped whole

struct Config {
    std::string host;  // an IPv4 address or a host name the robot can resolve
    int port = kDefaultPort;
    std::string token;
    bool valid() const;
};

// Host: letters, digits, '.', '-' (an IPv4 address or a DNS name), 1..kHostMax
bool validHost(std::string_view host);
// Token: letters, digits, '-', '_', '.', '~', '+', '/', '=' (what goes in a bearer header), kTokenMin..kTokenMax
bool validToken(std::string_view token);
// A POSIX TZ string as SETUP keeps them ("CST-8", "CET-1CEST,M3.5.0,M10.5.0/3", "<+0545>-5:45"): letters, digits and
// + - , . : / < >, 3..63
bool validTimezone(std::string_view tz);

// A line from the console:
//   "stackpet usage <host> <port> <token>"  -> Set, out filled
//   "stackpet usage clear"                  -> Clear
//   "stackpet usage show"                   -> Show (the host and port, the token's length only)
//   "stackpet usage style <key>"            -> Style, value: the style's key (usage_look.h: claude, rings, ...)
//   "stackpet usage tz <posix>"             -> Timezone, value: the Mac's TZ string
// Other "stackpet usage ..." lines (a bad host, port, token, style or TZ, a word too many) -> Bad; anything else ->
// None (the console carries nothing else for us). Spaces and a trailing '\r' are fine
enum class Command { None, Set, Clear, Show, Style, Timezone, Bad };
Command parseCommand(std::string_view line, Config& out, std::string* value = nullptr);

}  // namespace stackpet::usage
