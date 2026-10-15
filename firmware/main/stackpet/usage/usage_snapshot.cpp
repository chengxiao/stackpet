/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "usage_snapshot.h"
#include <ArduinoJson.h>
#include <algorithm>
#include <cctype>
#include <cmath>

namespace stackpet::usage {

namespace {

int read_pct(JsonVariantConst v)
{
    if (v.is<int64_t>()) {
        return static_cast<int>(std::clamp<int64_t>(v.as<int64_t>(), 0, 100));
    }
    if (v.is<double>()) {
        const double d = v.as<double>();
        return std::isfinite(d) ? static_cast<int>(std::lround(std::clamp(d, 0.0, 100.0))) : -1;
    }
    return -1;
}

int64_t read_time(JsonVariantConst v)
{
    if (v.is<int64_t>()) {
        return std::max<int64_t>(v.as<int64_t>(), 0);
    }
    if (v.is<double>()) {
        const double d = v.as<double>();
        return std::isfinite(d) && d > 0 && d < 1e12 ? static_cast<int64_t>(d) : 0;
    }
    return 0;
}

Window read_window(JsonVariantConst v)
{
    Window w;
    w.pct      = read_pct(v["pct"]);
    w.resetsAt = read_time(v["resetsAt"]);
    return w;
}

AgentState read_state(JsonVariantConst v)
{
    const char* s = v.as<const char*>();
    if (s == nullptr) {
        return AgentState::Unknown;
    }
    const std::string_view state = s;
    if (state == "idle") {
        return AgentState::Idle;
    }
    if (state == "work") {
        return AgentState::Work;
    }
    if (state == "wait") {
        return AgentState::Wait;
    }
    if (state == "done") {
        return AgentState::Done;
    }
    return AgentState::Unknown;
}

// Shown with the built-in font: printable ASCII only, and short
std::string read_model(JsonVariantConst v)
{
    const char* s = v.as<const char*>();
    std::string out;
    if (s == nullptr) {
        return out;
    }
    for (const char* p = s; *p != '\0' && out.size() < kModelMax; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c >= 0x20 && c < 0x7F) {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

bool equals_ignore_case(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

}  // namespace

bool parseSnapshot(std::string_view json, Snapshot& out)
{
    // Only what is shown: the snapshot also has the Mac's sensors, ChatGPT's limits and hourly token counts
    JsonDocument filter;
    filter["ts"]                           = true;
    filter["claude"]["session"]["pct"]     = true;
    filter["claude"]["session"]["resetsAt"] = true;
    filter["claude"]["weekly"]["pct"]      = true;
    filter["claude"]["weekly"]["resetsAt"] = true;
    filter["claude"]["dataAgeSec"]         = true;
    filter["claude"]["model"]              = true;
    filter["agent"]["state"]               = true;
    filter["agent"]["waiting"]             = true;

    JsonDocument doc;
    if (deserializeJson(doc, json.data(), json.size(), DeserializationOption::Filter(filter)) !=
        DeserializationError::Ok) {
        return false;
    }
    const JsonVariantConst claude = doc["claude"];
    if (!claude.is<JsonObjectConst>()) {
        return false;
    }
    Snapshot s;
    s.session = read_window(claude["session"]);
    s.weekly  = read_window(claude["weekly"]);
    s.model   = read_model(claude["model"]);
    const JsonVariantConst age = claude["dataAgeSec"];
    s.dataAgeSec               = age.is<int64_t>() ? static_cast<int>(std::clamp<int64_t>(age.as<int64_t>(), 0, 1 << 30))
                                                   : -1;
    s.state                    = read_state(doc["agent"]["state"]);
    const JsonVariantConst waiting = doc["agent"]["waiting"];
    s.waiting = waiting.is<int64_t>() ? static_cast<int>(std::clamp<int64_t>(waiting.as<int64_t>(), 0, 99)) : 0;
    s.ts      = read_time(doc["ts"]);
    out       = std::move(s);
    return true;
}

bool parseHttpResponse(std::string_view raw, HttpResponse& out)
{
    // "HTTP/1.1 200 OK\r\n"
    if (raw.size() < 12 || raw.substr(0, 7) != "HTTP/1." || raw[8] != ' ') {
        return false;
    }
    int status = 0;
    for (size_t i = 9; i < 12; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(raw[i]))) {
            return false;
        }
        status = status * 10 + (raw[i] - '0');
    }
    const size_t head_end = raw.find("\r\n\r\n");
    if (head_end == std::string_view::npos) {
        return false;
    }
    const std::string_view head = raw.substr(0, head_end);
    std::string_view body       = raw.substr(head_end + 4);

    // Content-Length, when there is one
    size_t line_start = head.find("\r\n");
    while (line_start != std::string_view::npos) {
        line_start += 2;
        const size_t line_end      = head.find("\r\n", line_start);
        const std::string_view line = head.substr(line_start, line_end == std::string_view::npos ? std::string_view::npos
                                                                                                 : line_end - line_start);
        const size_t colon = line.find(':');
        if (colon != std::string_view::npos && equals_ignore_case(line.substr(0, colon), "content-length")) {
            std::string_view value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                value.remove_prefix(1);
            }
            size_t length = 0;
            size_t digits = 0;
            for (const char c : value) {
                if (!std::isdigit(static_cast<unsigned char>(c)) || ++digits > 9) {
                    break;
                }
                length = length * 10 + static_cast<size_t>(c - '0');
            }
            if (digits == 0 || length > body.size()) {
                return false;  // no number, or the body cut short
            }
            body = body.substr(0, length);
            break;
        }
        line_start = line_end;
    }
    out.status = status;
    out.body   = body;
    return true;
}

}  // namespace stackpet::usage
