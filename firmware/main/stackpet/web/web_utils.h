/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

/**
 * Pure helpers of the LAN web server (no ESP-IDF / LVGL dependencies, host testable).
 */
namespace stackpet::web::utils {

// Decode %XX escapes and '+' (application/x-www-form-urlencoded). Malformed escapes are kept verbatim.
std::string urlDecode(std::string_view text);

// Value of `key` in a query string like "a=1&b=%E4%BD%A0" (URL decoded). nullopt if the key is absent.
std::optional<std::string> queryValue(std::string_view query, std::string_view key);

// Parse a number, clamp it into [minimum, maximum]. Returns fallback for empty / invalid / non-finite input.
float parseFloat(std::optional<std::string> text, float fallback, float minimum, float maximum);
int parseInt(std::optional<std::string> text, int fallback, int minimum, int maximum);

// Keep at most maxChars UTF-8 code points. Invalid / overlong sequences are dropped, control characters
// (newline, tab...) become spaces because the speech bubble is a single line.
std::string sanitizeUtf8(std::string_view text, size_t maxChars);

// Escape a string for a JSON string literal (without the surrounding quotes).
std::string jsonEscape(std::string_view text);

// Strict index parameter: only "0".."count-1" written as plain decimal digits (no sign, spaces, leading zeros or
// fractions). -1 if absent or invalid.
int parseStrictIndex(std::optional<std::string> text, int count);

// Name -> enum value mappings used by the HTTP API (case-insensitive). -1 if unknown.
int emotionFromName(std::string_view name);  // stackchan::avatar::Emotion order (10 values, kEmotionCount)
int sfxFromName(std::string_view name);      // stackpet::Sfx order
bool isDecoratorName(std::string_view name);  // heart | angry | sweat | shy | dizzy
bool isActionName(std::string_view name);     // nod | shake | spin | dance

// MJPEG (multipart/x-mixed-replace) framing. Only 32-bit printf formats are used: the firmware links newlib-nano,
// which cannot print 64-bit integers.
constexpr const char* kMjpegBoundary = "stackchanframe";
std::string mjpegContentType();
std::string mjpegPartHeader(size_t jpegLength, int64_t timestampUs);

// Access token for the LAN remote. `random` returns 32 random bits (esp_random on the device). The alphabet has no
// look-alike characters (0/o, 1/l/i) because the token may be typed by hand.
std::string makeToken(uint32_t (*random)(), size_t length);
// Constant-time comparison; an empty expected token never matches.
bool tokenMatches(std::string_view expected, std::string_view provided);

}  // namespace stackpet::web::utils
