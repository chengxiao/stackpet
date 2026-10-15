/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "usage_config.h"
#include "usage_look.h"
#include <cctype>
#include <vector>

namespace stackpet::usage {

namespace {

std::vector<std::string_view> split_words(std::string_view line)
{
    std::vector<std::string_view> words;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) {
            ++i;
        }
        const size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') {
            ++i;
        }
        if (i > start) {
            words.push_back(line.substr(start, i - start));
        }
    }
    return words;
}

bool parse_port(std::string_view s, int& port)
{
    if (s.empty() || s.size() > 5) {
        return false;
    }
    int value = 0;
    for (const char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    if (value < 1 || value > 65535) {
        return false;
    }
    port = value;
    return true;
}

}  // namespace

bool Config::valid() const
{
    return validHost(host) && port >= 1 && port <= 65535 && validToken(token);
}

bool validHost(std::string_view host)
{
    if (host.empty() || host.size() > kHostMax || host.front() == '.' || host.front() == '-') {
        return false;
    }
    for (const char c : host) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-') {
            return false;
        }
    }
    return true;
}

bool validToken(std::string_view token)
{
    if (token.size() < kTokenMin || token.size() > kTokenMax) {
        return false;
    }
    for (const char c : token) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~' ||
                        c == '+' || c == '/' || c == '=';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool validTimezone(std::string_view tz)
{
    if (tz.size() < 3 || tz.size() > 63) {
        return false;
    }
    for (const char c : tz) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '-' || c == ',' || c == '.' ||
                        c == ':' || c == '/' || c == '<' || c == '>';
        if (!ok) {
            return false;
        }
    }
    return true;
}

Command parseCommand(std::string_view line, Config& out, std::string* value)
{
    const auto words = split_words(line);
    if (words.size() < 2 || words[0] != "stackpet" || words[1] != "usage") {
        return Command::None;
    }
    if (words.size() == 3 && words[2] == "clear") {
        return Command::Clear;
    }
    if (words.size() == 3 && words[2] == "show") {
        return Command::Show;
    }
    if (words.size() == 4 && words[2] == "style") {
        Style style;
        if (!parseStyle(words[3], style)) {
            return Command::Bad;
        }
        if (value != nullptr) {
            *value = std::string(words[3]);
        }
        return Command::Style;
    }
    if (words.size() == 4 && words[2] == "tz") {
        if (!validTimezone(words[3])) {
            return Command::Bad;
        }
        if (value != nullptr) {
            *value = std::string(words[3]);
        }
        return Command::Timezone;
    }
    if (words.size() != 5) {
        return Command::Bad;
    }
    Config c;
    c.host  = std::string(words[2]);
    c.token = std::string(words[4]);
    if (!parse_port(words[3], c.port) || !c.valid()) {
        return Command::Bad;
    }
    out = std::move(c);
    return Command::Set;
}

}  // namespace stackpet::usage
