/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stackchan/avatar/avatar/elements/emotion.h>
#include <cstdint>

namespace stackpet::face::bigeyes {

/* --------------------------------- Layout --------------------------------- */
// Offsets from the panel centre (320 x 240 screen). The speech bubble starts at about y = +58.
constexpr int kEyeX   = 62;   // eye centre x (left eye -kEyeX, right eye +kEyeX)
constexpr int kEyeY   = -14;  // eye centre y
constexpr int kMouthY = 46;   // mouth centre y
constexpr int kMoveX  = 16;   // setPosition(+-100) moves a feature by +-16 px, like the official skin
constexpr int kMoveY  = 16;

// Every eye shape collapses onto this bar when blinking shut / when it is swapped for another shape
constexpr float kBarW = 76.0f;
constexpr float kBarH = 10.0f;

/* --------------------------------- Colours -------------------------------- */
constexpr uint32_t kColorCyan     = 0x3CE1FF;
constexpr uint32_t kColorSleepy   = 0x2196AE;  // dim cyan
constexpr uint32_t kColorAngry    = 0xFF5A32;
constexpr uint32_t kColorSad      = 0x5A8CFF;
constexpr uint32_t kColorShy      = 0xFF8CC8;
constexpr uint32_t kColorLove     = 0xFF3C82;
constexpr uint32_t kColorBlush    = 0xFF4F96;
constexpr uint32_t kColorTear     = 0x9CCBFF;
constexpr uint32_t kColorZzz      = 0x4FC6E0;

uint32_t emotionColor(stackchan::avatar::Emotion emotion);

/* ---------------------------------- Shapes -------------------------------- */
enum class EyeKind : uint8_t {
    Body,     // rounded rectangle, optionally with a diagonal cut off the inner / outer top corner
    Arc,      // thick upside-down U (happy / shy smiling eyes)
    Chevron,  // thick > < pointing to the face centre (laughing)
    Heart,
};

/**
 * Geometry of one eye at full size, in px. "Inner" is the side facing the centre of the face, so the right eye is
 * the left eye mirrored. Body shapes of different emotions are interpolated field by field (morph); any other
 * change of kind goes through the closed bar (squash, swap, pop open).
 */
struct EyeShape {
    EyeKind kind = EyeKind::Body;
    // Body
    float w        = 76.0f;
    float h        = 92.0f;
    float r        = 24.0f;
    float dy       = 0.0f;   // shape centre below the eye centre (all kinds)
    float cutIn    = 0.0f;   // drop of the inner top corner cut, measured at the inner edge
    float cutOut   = 0.0f;   // drop of the outer top corner cut, measured at the outer edge
    float cutStart = 0.25f;  // the cut line leaves the top edge this far (fraction of w) from the opposite side
    // Arc: radius of the stroke centre line, half of the swept angle (deg)
    float arcR    = 30.0f;
    float arcHalf = 90.0f;
    // Chevron: half width / half height of the stroke centre line
    float chW = 28.0f;
    float chH = 30.0f;
    // Arc / Chevron stroke width
    float thick = 18.0f;
    // Heart: full width
    float heartW = 88.0f;
};

EyeShape eyeShape(stackchan::avatar::Emotion emotion, bool leftEye);
EyeShape lerpShape(const EyeShape& a, const EyeShape& b, float t);

/* ---------------------------------- Easing -------------------------------- */
inline float clamp01(float t)
{
    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}
inline float lerpf(float a, float b, float t)
{
    return a + (b - a) * t;
}
inline float easeIn(float t)
{
    return t * t;
}
inline float easeOut(float t)
{
    return 1.0f - (1.0f - t) * (1.0f - t);
}
inline float easeInOut(float t)
{
    return t < 0.5f ? 2.0f * t * t : 1.0f - 2.0f * (1.0f - t) * (1.0f - t);
}
// Ends at 1 after overshooting by about 5 %
inline float easeOutBack(float t)
{
    const float s = 1.2f;
    float u       = t - 1.0f;
    return 1.0f + (s + 1.0f) * u * u * u + s * u * u;
}

}  // namespace stackpet::face::bigeyes
