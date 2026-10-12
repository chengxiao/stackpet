/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "web_utils.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace stackpet::web::utils {

namespace {

int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool equals_ignore_case(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); i++) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = static_cast<char>(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = static_cast<char>(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

template <size_t N>
int index_of(const std::array<std::string_view, N>& names, std::string_view name)
{
    for (size_t i = 0; i < N; i++) {
        if (equals_ignore_case(names[i], name)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Same order as stackchan::avatar::Emotion (the last 4 were appended by StackPet)
constexpr std::array<std::string_view, 10> kEmotionNames = {"neutral", "happy",    "angry",     "sad", "doubt",
                                                            "sleepy",  "laughing", "surprised", "shy", "love"};

// Same order as stackpet::Sfx
// Same order as stackpet::Sfx
constexpr std::array<std::string_view, 13> kSfxNames = {"click",  "start",  "tick", "go",       "success",
                                                        "fail",   "caught", "win",  "reveal",   "boop",
                                                        "mutouren", "radarping", "alarm"};

constexpr std::array<std::string_view, 5> kDecoratorNames = {"heart", "angry", "sweat", "shy", "dizzy"};
constexpr std::array<std::string_view, 4> kActionNames    = {"nod", "shake", "spin", "dance"};

}  // namespace

std::string urlDecode(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++) {
        char c = text[i];
        if (c == '+') {
            out.push_back(' ');
        } else if (c == '%' && i + 2 < text.size()) {
            int hi = hex_value(text[i + 1]);
            int lo = hex_value(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
            } else {
                out.push_back(c);
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::optional<std::string> queryValue(std::string_view query, std::string_view key)
{
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t end = query.find('&', pos);
        if (end == std::string_view::npos) {
            end = query.size();
        }
        std::string_view pair = query.substr(pos, end - pos);
        size_t eq             = pair.find('=');
        std::string_view name = eq == std::string_view::npos ? pair : pair.substr(0, eq);
        if (!name.empty() && urlDecode(name) == key) {
            return eq == std::string_view::npos ? std::string() : urlDecode(pair.substr(eq + 1));
        }
        if (end == query.size()) {
            break;
        }
        pos = end + 1;
    }
    return std::nullopt;
}

float parseFloat(std::optional<std::string> text, float fallback, float minimum, float maximum)
{
    if (!text || text->empty()) {
        return fallback;
    }
    char* end   = nullptr;
    float value = std::strtof(text->c_str(), &end);
    if (end == text->c_str() || *end != '\0' || !std::isfinite(value)) {
        return fallback;
    }
    return std::clamp(value, minimum, maximum);
}

int parseInt(std::optional<std::string> text, int fallback, int minimum, int maximum)
{
    if (!text || text->empty()) {
        return fallback;
    }
    char* end  = nullptr;
    long value = std::strtol(text->c_str(), &end, 10);
    if (end == text->c_str() || *end != '\0') {
        return fallback;
    }
    value = std::clamp<long>(value, minimum, maximum);
    return static_cast<int>(value);
}

std::string sanitizeUtf8(std::string_view text, size_t maxChars)
{
    std::string out;
    out.reserve(std::min(text.size(), maxChars * 4));
    size_t chars = 0;
    size_t i     = 0;
    while (i < text.size() && chars < maxChars) {
        auto c = static_cast<unsigned char>(text[i]);
        size_t len;
        uint32_t cp;
        if (c < 0x80) {
            len = 1;
            cp  = c;
        } else if ((c & 0xE0) == 0xC0) {
            len = 2;
            cp  = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
            cp  = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
            cp  = c & 0x07;
        } else {
            i++;  // Invalid lead byte
            continue;
        }
        if (i + len > text.size()) {
            break;  // Truncated sequence at the end
        }
        bool valid = true;
        for (size_t k = 1; k < len; k++) {
            auto cc = static_cast<unsigned char>(text[i + k]);
            if ((cc & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        // Reject overlong encodings, surrogates and out of range code points
        static constexpr uint32_t kMinForLength[5] = {0, 0, 0x80, 0x800, 0x10000};
        if (!valid || cp < kMinForLength[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            i++;
            continue;
        }
        if (cp < 0x20 || cp == 0x7F) {
            out.push_back(' ');  // Control characters (newline, tab...) become spaces: the bubble is single line
        } else {
            out.append(text.substr(i, len));
        }
        chars++;
        i += len;
    }
    return out;
}

std::string jsonEscape(std::string_view text)
{
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    return out;
}

int parseStrictIndex(std::optional<std::string> text, int count)
{
    if (!text || text->empty() || text->size() > 9 || count <= 0) {
        return -1;
    }
    if (text->size() > 1 && (*text)[0] == '0') {
        return -1;  // "01", "00"
    }
    int value = 0;
    for (char c : *text) {
        if (c < '0' || c > '9') {
            return -1;
        }
        value = value * 10 + (c - '0');
    }
    return value < count ? value : -1;
}

int emotionFromName(std::string_view name)
{
    return index_of(kEmotionNames, name);
}

int sfxFromName(std::string_view name)
{
    return index_of(kSfxNames, name);
}

bool isDecoratorName(std::string_view name)
{
    return index_of(kDecoratorNames, name) >= 0;
}

bool isActionName(std::string_view name)
{
    return index_of(kActionNames, name) >= 0;
}

std::string mjpegContentType()
{
    return std::string("multipart/x-mixed-replace;boundary=") + kMjpegBoundary;
}

std::string mjpegPartHeader(size_t jpegLength, int64_t timestampUs)
{
    if (timestampUs < 0) {
        timestampUs = 0;
    }
    // 32-bit seconds cover 136 years of uptime
    const auto seconds = static_cast<unsigned>(timestampUs / 1000000);
    const auto micros  = static_cast<unsigned>(timestampUs % 1000000);
    char buf[160];
    int n = std::snprintf(buf, sizeof(buf),
                          "\r\n--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp: %u.%06u\r\n\r\n",
                          kMjpegBoundary, static_cast<unsigned>(jpegLength), seconds, micros);
    return std::string(buf, n > 0 ? static_cast<size_t>(n) : 0);
}

std::string makeToken(uint32_t (*random)(), size_t length)
{
    static constexpr char kAlphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
    static constexpr uint32_t kAlphabetSize = sizeof(kAlphabet) - 1;
    std::string token;
    token.reserve(length);
    for (size_t i = 0; i < length; i++) {
        token.push_back(kAlphabet[random() % kAlphabetSize]);
    }
    return token;
}

bool tokenMatches(std::string_view expected, std::string_view provided)
{
    if (expected.empty()) {
        return false;
    }
    // Do not leak the position of the first wrong character through the timing
    unsigned diff = static_cast<unsigned>(expected.size() ^ provided.size());
    for (size_t i = 0; i < expected.size(); i++) {
        char p = i < provided.size() ? provided[i] : '\0';
        diff |= static_cast<unsigned>(static_cast<unsigned char>(expected[i]) ^ static_cast<unsigned char>(p));
    }
    return diff == 0;
}

}  // namespace stackpet::web::utils
