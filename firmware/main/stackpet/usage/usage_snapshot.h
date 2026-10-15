/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
// StackPet: the Claude usage the launcher's screensaver shows. It comes from the Mac's ClaudeUsageBar app, its LAN
// service (GET /api/v1/snapshot, port 8787 by default, a bearer token). Pure: parsing only, host tested
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace stackpet::usage {

enum class AgentState { Unknown, Idle, Work, Wait, Done };

// One of Claude's limits: how much of it is used, and when it starts over
struct Window {
    int pct          = -1;  // 0..100, -1: not known
    int64_t resetsAt = 0;   // Unix seconds, 0: not known
};

struct Snapshot {
    Window session;  // the 5-hour window
    Window weekly;
    AgentState state = AgentState::Unknown;  // Claude Code's (or Codex's) sessions on the Mac, all of them
    int waiting      = 0;                    // sessions waiting for the user
    int64_t ts       = 0;                    // the Mac's clock when it answered, Unix seconds (0: not given)
    int dataAgeSec   = -1;                   // how old the Mac's Claude numbers are (-1: not given)
    std::string model;                       // e.g. "Opus 5.5": printable ASCII only, at most kModelMax
};
constexpr size_t kModelMax = 24;

// The snapshot's JSON (only the fields above are kept; numbers out of range are clamped, wrong types read as not
// known). False when it is not JSON or has no "claude" object: out is left as it was
bool parseSnapshot(std::string_view json, Snapshot& out);

// An HTTP/1.x response as it came off the socket: the status code and the body. The body is Content-Length long when
// the header says so (false if fewer bytes came), else the rest. False when there is no status line or header end
struct HttpResponse {
    int status = 0;
    std::string_view body;  // into raw
};
bool parseHttpResponse(std::string_view raw, HttpResponse& out);

}  // namespace stackpet::usage
