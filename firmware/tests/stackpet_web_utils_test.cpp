/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <stackchan/avatar/avatar/elements/emotion.h>
#include <stackpet/sound/sound_player.h>
#include <stackpet/web/web_utils.h>

using namespace stackpet::web::utils;

namespace {

int failures = 0;

void expectTrue(bool condition, const char* label)
{
    if (!condition) {
        std::cerr << "FAILED: " << label << '\n';
        failures++;
    }
}

void expectEqual(const std::string& actual, const std::string& expected, const char* label)
{
    if (actual != expected) {
        std::cerr << "FAILED: " << label << ": expected [" << expected << "], got [" << actual << "]\n";
        failures++;
    }
}

void expectEqual(int actual, int expected, const char* label)
{
    if (actual != expected) {
        std::cerr << "FAILED: " << label << ": expected " << expected << ", got " << actual << '\n';
        failures++;
    }
}

void expectNear(float actual, float expected, const char* label)
{
    if (std::fabs(actual - expected) > 1e-5f) {
        std::cerr << "FAILED: " << label << ": expected " << expected << ", got " << actual << '\n';
        failures++;
    }
}

void testUrlDecode()
{
    expectEqual(urlDecode("hello+world"), "hello world", "plus to space");
    expectEqual(urlDecode("%E4%BD%A0%E5%A5%BD"), "你好", "utf8 escapes");
    expectEqual(urlDecode("100%25"), "100%", "escaped percent");
    expectEqual(urlDecode("bad%G1"), "bad%G1", "invalid escape kept");
    expectEqual(urlDecode("tail%4"), "tail%4", "truncated escape kept");
    expectEqual(urlDecode(""), "", "empty");
}

void testQueryValue()
{
    auto q = std::string_view("x=0.5&y=-1&speed=700&t=%E4%BD%A0");
    expectEqual(queryValue(q, "x").value_or("?"), "0.5", "first key");
    expectEqual(queryValue(q, "y").value_or("?"), "-1", "middle key");
    expectEqual(queryValue(q, "speed").value_or("?"), "700", "last numeric key");
    expectEqual(queryValue(q, "t").value_or("?"), "你", "decoded value");
    expectTrue(!queryValue(q, "z").has_value(), "missing key");
    expectEqual(queryValue("xx=1&x=2", "x").value_or("?"), "2", "no prefix collision");
    expectEqual(queryValue("flag&a=1", "flag").value_or("?"), "", "key without value");
    expectTrue(!queryValue("", "a").has_value(), "empty query");
}

void testParseNumbers()
{
    expectNear(parseFloat(std::string("0.25"), 0.0f, -1.0f, 1.0f), 0.25f, "float ok");
    expectNear(parseFloat(std::string("3"), 0.0f, -1.0f, 1.0f), 1.0f, "float clamp high");
    expectNear(parseFloat(std::string("-9.5"), 0.0f, -1.0f, 1.0f), -1.0f, "float clamp low");
    expectNear(parseFloat(std::string("abc"), 0.5f, -1.0f, 1.0f), 0.5f, "float invalid");
    expectNear(parseFloat(std::string("1.0x"), 0.5f, -1.0f, 1.0f), 0.5f, "float trailing garbage");
    expectNear(parseFloat(std::string("nan"), 0.5f, -1.0f, 1.0f), 0.5f, "float nan");
    expectNear(parseFloat(std::nullopt, 0.5f, -1.0f, 1.0f), 0.5f, "float missing");
    expectEqual(parseInt(std::string("700"), 500, 0, 1000), 700, "int ok");
    expectEqual(parseInt(std::string("5000"), 500, 0, 1000), 1000, "int clamp");
    expectEqual(parseInt(std::string("-3"), 500, 0, 255), 0, "int clamp low");
    expectEqual(parseInt(std::string(""), 500, 0, 1000), 500, "int empty");
    expectEqual(parseInt(std::string("12a"), 500, 0, 1000), 500, "int garbage");
}

void testStrictIndex()
{
    expectEqual(parseStrictIndex(std::string("0"), 3), 0, "index 0");
    expectEqual(parseStrictIndex(std::string("2"), 3), 2, "index last");
    expectEqual(parseStrictIndex(std::string("3"), 3), -1, "index out of range");
    expectEqual(parseStrictIndex(std::string("01"), 3), -1, "leading zero");
    expectEqual(parseStrictIndex(std::string("-1"), 3), -1, "negative");
    expectEqual(parseStrictIndex(std::string("+1"), 3), -1, "plus sign");
    expectEqual(parseStrictIndex(std::string("1.0"), 3), -1, "fraction");
    expectEqual(parseStrictIndex(std::string(" 1"), 3), -1, "space");
    expectEqual(parseStrictIndex(std::string("1x"), 3), -1, "garbage");
    expectEqual(parseStrictIndex(std::string(""), 3), -1, "empty");
    expectEqual(parseStrictIndex(std::nullopt, 3), -1, "missing");
    expectEqual(parseStrictIndex(std::string("99999999999"), 3), -1, "overflow length");
    expectEqual(parseStrictIndex(std::string("1"), 0), -1, "empty range");
}

void testSanitizeUtf8()
{
    expectEqual(sanitizeUtf8("你好世界", 2), "你好", "truncate by code points");
    expectEqual(sanitizeUtf8("abc", 10), "abc", "ascii untouched");
    expectEqual(sanitizeUtf8("a\nb\tc", 10), "a b c", "control chars to spaces");
    expectEqual(sanitizeUtf8("ok\xff\xfe!", 10), "ok!", "invalid bytes dropped");
    expectEqual(sanitizeUtf8("\xc0\xaf", 10), "", "overlong rejected");
    expectEqual(sanitizeUtf8("x\xe4\xbd", 10), "x", "truncated sequence dropped");
    expectEqual(sanitizeUtf8("😀ab", 2), "😀a", "4-byte sequence");
    std::string long_text(200, 'a');
    expectEqual(static_cast<int>(sanitizeUtf8(long_text, 60).size()), 60, "max chars");
}

void testJsonEscape()
{
    expectEqual(jsonEscape("a\"b\\c"), "a\\\"b\\\\c", "quote and backslash");
    expectEqual(jsonEscape("l1\nl2"), "l1\\nl2", "newline");
    expectEqual(jsonEscape(std::string("\x01", 1)), "\\u0001", "control char");
    expectEqual(jsonEscape("192.168.1.2"), "192.168.1.2", "plain");
}

void testNameMappings()
{
    using stackchan::avatar::Emotion;
    expectEqual(emotionFromName("neutral"), static_cast<int>(Emotion::Neutral), "emotion neutral");
    expectEqual(emotionFromName("Happy"), static_cast<int>(Emotion::Happy), "emotion case-insensitive");
    expectEqual(emotionFromName("sleepy"), static_cast<int>(Emotion::Sleepy), "emotion sleepy");
    expectEqual(emotionFromName("laughing"), static_cast<int>(Emotion::Laughing), "emotion laughing");
    expectEqual(emotionFromName("surprised"), static_cast<int>(Emotion::Surprised), "emotion surprised");
    expectEqual(emotionFromName("shy"), static_cast<int>(Emotion::Shy), "emotion shy");
    expectEqual(emotionFromName("LOVE"), static_cast<int>(Emotion::Love), "emotion love");
    expectEqual(stackchan::avatar::kEmotionCount, 10, "emotion count");
    expectEqual(emotionFromName("excited"), -1, "emotion unknown");

    using stackpet::Sfx;
    expectEqual(sfxFromName("click"), static_cast<int>(Sfx::Click), "sfx click");
    expectEqual(sfxFromName("tick"), static_cast<int>(Sfx::Tick), "sfx tick");
    expectEqual(sfxFromName("caught"), static_cast<int>(Sfx::Caught), "sfx caught");
    expectEqual(sfxFromName("boop"), static_cast<int>(Sfx::Boop), "sfx boop");
    expectEqual(sfxFromName("MuTouRen"), static_cast<int>(Sfx::MuTouRen), "sfx mutouren");
    expectEqual(sfxFromName("radarping"), static_cast<int>(Sfx::RadarPing), "sfx radarping");
    expectEqual(sfxFromName("alarm"), static_cast<int>(Sfx::Alarm), "sfx alarm");
    expectEqual(sfxFromName("explode"), -1, "sfx unknown");

    expectTrue(isDecoratorName("heart") && isDecoratorName("dizzy"), "decorators");
    expectTrue(!isDecoratorName("star"), "decorator unknown");
    expectTrue(isActionName("nod") && isActionName("spin") && isActionName("dance"), "actions");
    expectTrue(!isActionName("jump"), "action unknown");
}

void testMjpeg()
{
    expectEqual(mjpegContentType(), "multipart/x-mixed-replace;boundary=stackchanframe", "content type");
    auto header = mjpegPartHeader(1234, 5000123);
    expectTrue(header.rfind("\r\n--stackchanframe\r\n", 0) == 0, "part starts with boundary");
    expectTrue(header.find("Content-Type: image/jpeg\r\n") != std::string::npos, "part content type");
    expectTrue(header.find("Content-Length: 1234\r\n") != std::string::npos, "part length");
    expectTrue(header.find("X-Timestamp: 5.000123\r\n") != std::string::npos, "part timestamp");
    expectTrue(header.size() > 4 && header.compare(header.size() - 4, 4, "\r\n\r\n") == 0, "part header ends");
    // Long uptime: still printed with 32-bit formats (newlib-nano on the device has no %lld)
    auto later = mjpegPartHeader(15000, 86400LL * 400 * 1000000 + 42);
    expectTrue(later.find("X-Timestamp: 34560000.000042\r\n") != std::string::npos, "400 days timestamp");
    expectTrue(later.find("Content-Length: 15000\r\n") != std::string::npos, "length after long uptime");
    expectTrue(mjpegPartHeader(1, -5).find("X-Timestamp: 0.000000\r\n") != std::string::npos, "negative clamps");
}

uint32_t g_random_state = 1;
uint32_t fakeRandom()
{
    g_random_state = g_random_state * 1664525u + 1013904223u;
    return g_random_state;
}

void testToken()
{
    g_random_state = 7;
    auto a = makeToken(fakeRandom, 8);
    auto b = makeToken(fakeRandom, 8);
    expectEqual(static_cast<int>(a.size()), 8, "token length");
    expectTrue(a != b, "tokens differ");
    for (char c : a + b) {
        bool lower = c >= 'a' && c <= 'z';
        bool digit = c >= '2' && c <= '9';
        expectTrue(lower || digit, "token alphabet");
        expectTrue(c != 'l' && c != 'o' && c != 'i' && c != '0' && c != '1', "no look-alike characters");
    }
    expectTrue(tokenMatches("abc12345", "abc12345"), "token match");
    expectTrue(!tokenMatches("abc12345", "abc12346"), "token mismatch last char");
    expectTrue(!tokenMatches("abc12345", "abc1234"), "token shorter");
    expectTrue(!tokenMatches("abc12345", "abc123456"), "token longer");
    expectTrue(!tokenMatches("abc12345", ""), "token empty provided");
    expectTrue(!tokenMatches("", ""), "empty expected never matches");
}

}  // namespace

int main()
{
    testUrlDecode();
    testQueryValue();
    testParseNumbers();
    testStrictIndex();
    testSanitizeUtf8();
    testJsonEscape();
    testNameMappings();
    testMjpeg();
    testToken();
    if (failures > 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "stackpet_web_utils_test: all checks passed\n";
    return 0;
}
