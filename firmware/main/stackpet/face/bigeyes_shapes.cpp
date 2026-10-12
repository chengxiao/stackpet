/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "bigeyes_shapes.h"

using stackchan::avatar::Emotion;

namespace stackpet::face::bigeyes {

uint32_t emotionColor(Emotion emotion)
{
    switch (emotion) {
        case Emotion::Angry:
            return kColorAngry;
        case Emotion::Sad:
            return kColorSad;
        case Emotion::Sleepy:
            return kColorSleepy;
        case Emotion::Shy:
            return kColorShy;
        case Emotion::Love:
            return kColorLove;
        case Emotion::Neutral:
        case Emotion::Happy:
        case Emotion::Laughing:
        case Emotion::Surprised:
        case Emotion::Doubt:
        default:
            return kColorCyan;
    }
}

static EyeShape body(float w, float h, float r, float dy)
{
    EyeShape s;
    s.kind = EyeKind::Body;
    s.w    = w;
    s.h    = h;
    s.r    = r;
    s.dy   = dy;
    return s;
}

EyeShape eyeShape(Emotion emotion, bool leftEye)
{
    switch (emotion) {
        case Emotion::Happy: {
            EyeShape s;
            s.kind    = EyeKind::Arc;
            s.arcR    = 29.0f;
            s.arcHalf = 90.0f;
            s.thick   = 20.0f;
            s.dy      = 0.0f;
            return s;
        }
        case Emotion::Laughing: {
            EyeShape s;
            s.kind  = EyeKind::Chevron;
            s.chW   = 27.0f;
            s.chH   = 30.0f;
            s.thick = 20.0f;
            return s;
        }
        case Emotion::Angry: {
            // Squinting down, the inner top corner sliced off: a frown
            EyeShape s = body(78.0f, 82.0f, 22.0f, 8.0f);
            s.cutIn    = 44.0f;
            s.cutStart = 0.18f;
            return s;
        }
        case Emotion::Sad: {
            // The outer top corner sliced off: drooping eyes
            EyeShape s = body(76.0f, 84.0f, 24.0f, 8.0f);
            s.cutOut   = 36.0f;
            s.cutStart = 0.30f;
            return s;
        }
        case Emotion::Surprised:
            return body(96.0f, 96.0f, 48.0f, -4.0f);
        case Emotion::Doubt:
            // One eye wide open, the other one squinting
            return leftEye ? body(80.0f, 96.0f, 26.0f, -6.0f) : body(76.0f, 40.0f, 20.0f, 14.0f);
        case Emotion::Sleepy:
            return body(76.0f, 12.0f, 6.0f, 18.0f);
        case Emotion::Shy: {
            EyeShape s;
            s.kind    = EyeKind::Arc;
            s.arcR    = 26.0f;
            s.arcHalf = 78.0f;
            s.thick   = 17.0f;
            s.dy      = 2.0f;
            return s;
        }
        case Emotion::Love: {
            // A little smaller and lower than the other eyes: the HeartDecorator the apps add floats at the upper
            // right (108, -70) and must not touch the right heart, even at the peak of its heartbeat
            EyeShape s;
            s.kind   = EyeKind::Heart;
            s.heartW = 84.0f;
            s.dy     = 8.0f;
            return s;
        }
        case Emotion::Neutral:
        default:
            return body(76.0f, 92.0f, 24.0f, 0.0f);
    }
}

EyeShape lerpShape(const EyeShape& a, const EyeShape& b, float t)
{
    EyeShape s;
    s.kind     = t < 0.5f ? a.kind : b.kind;
    s.w        = lerpf(a.w, b.w, t);
    s.h        = lerpf(a.h, b.h, t);
    s.r        = lerpf(a.r, b.r, t);
    s.dy       = lerpf(a.dy, b.dy, t);
    s.cutIn    = lerpf(a.cutIn, b.cutIn, t);
    s.cutOut   = lerpf(a.cutOut, b.cutOut, t);
    // Keep the cut angle of the shape that actually has a cut, so it grows in instead of swinging around
    if (a.cutIn + a.cutOut < 0.5f) {
        s.cutStart = b.cutStart;
    } else if (b.cutIn + b.cutOut < 0.5f) {
        s.cutStart = a.cutStart;
    } else {
        s.cutStart = lerpf(a.cutStart, b.cutStart, t);
    }
    s.arcR    = lerpf(a.arcR, b.arcR, t);
    s.arcHalf = lerpf(a.arcHalf, b.arcHalf, t);
    s.chW     = lerpf(a.chW, b.chW, t);
    s.chH     = lerpf(a.chH, b.chH, t);
    s.thick   = lerpf(a.thick, b.thick, t);
    s.heartW  = lerpf(a.heartW, b.heartW, t);
    return s;
}

}  // namespace stackpet::face::bigeyes
