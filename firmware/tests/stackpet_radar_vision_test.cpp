/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
// Host tests of the RADAR vision helpers: FaceTracker and boxesToFaces (stackpet/vision/face_detector.*), the face
// model's scoring and internal RAM budget (stackpet/vision/face_scoring.h), the YUYV -> RGB565 converter of the
// FrameGrabber (stackpet/camera/frame_grabber.h) and the radar sound effects (tone_synth)
#include "stackpet_games_check.h"
#include <stackpet/camera/frame_grabber.h>
#include <stackpet/sound/tone_synth.h>
#include <stackpet/vision/face_detector.h>
#include <stackpet/vision/face_scoring.h>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace stackpet;
using namespace stackpet::vision;

namespace {

Face face(float cx, float cy, float w = 0.2f, float score = 0.9f)
{
    Face f;
    f.cx    = cx;
    f.cy    = cy;
    f.w     = w;
    f.h     = w * 1.2f;
    f.score = score;
    return f;
}

void test_tracker_hits_and_smoothing()
{
    FaceTracker tracker;
    auto t = tracker.update({}, 0);
    CHECK(!t.present);
    CHECK(t.hits == 0);

    t = tracker.update({face(0.5f, 0.0f)}, 100);
    CHECK(t.present);
    CHECK(t.hits == 1);
    CHECK_NEAR(t.face.cx, 0.5f, 1e-6f);

    // The next detection moved: the smoothed box follows part of the way, the raw box all the way
    t = tracker.update({face(0.3f, 0.1f)}, 200);
    CHECK(t.hits == 2);
    CHECK(t.face.cx < 0.5f && t.face.cx > 0.3f);
    CHECK_NEAR(t.face.cx, 0.5f + 0.6f * (0.3f - 0.5f), 1e-5f);
    CHECK_NEAR(t.raw.cx, 0.3f, 1e-6f);
    CHECK(t.lastSeenMs == 200);
}

void test_tracker_keeps_its_target()
{
    FaceTracker tracker;
    tracker.update({face(-0.5f, 0.0f, 0.2f, 0.7f)}, 0);
    // A better face elsewhere does not steal the track while the tracked one is still matched
    auto t = tracker.update({face(0.8f, 0.0f, 0.2f, 0.99f), face(-0.45f, 0.05f, 0.2f, 0.7f)}, 100);
    CHECK(t.hits == 2);
    CHECK(t.raw.cx < 0.0f);
}

void test_tracker_miss_breaks_the_streak()
{
    FaceTracker tracker;
    tracker.update({face(0.0f, 0.0f)}, 0);
    tracker.update({face(0.0f, 0.0f)}, 100);
    auto t = tracker.update({}, 200);
    // One miss: still tracked (for "is the target still there"), but the hit streak starts over
    CHECK(t.present);
    CHECK(t.hits == 0);
    CHECK(t.misses == 1);
    t = tracker.update({face(0.05f, 0.0f)}, 300);
    CHECK(t.present);
    CHECK(t.hits == 1);  // consecutive hits only: hit, miss, hit is not a confirmation
    CHECK(t.misses == 0);
}

void test_tracker_drops_a_lost_target()
{
    FaceTracker::Config config;
    config.maxMisses = 2;
    FaceTracker tracker(config);
    tracker.update({face(0.0f, 0.0f)}, 0);
    tracker.update({}, 100);
    tracker.update({}, 200);
    auto t = tracker.update({}, 300);
    CHECK(!t.present);

    // Too old is lost too, even within maxMisses
    tracker.update({face(0.0f, 0.0f)}, 1000);
    t = tracker.update({}, 3000);
    CHECK(!t.present);
}

void test_tracker_gate_and_new_track()
{
    FaceTracker tracker;
    tracker.update({face(-0.8f, 0.0f)}, 0);
    // Far away (beyond the gate): not the same target, a miss
    auto t = tracker.update({face(0.8f, 0.0f)}, 100);
    CHECK(t.hits == 0);
    CHECK(t.misses == 1);
    tracker.update({face(0.8f, 0.0f)}, 200);
    // Third miss: the old track is dropped and the other face starts a new one in the same run
    t = tracker.update({face(0.8f, 0.0f)}, 300);
    CHECK(t.present);
    CHECK(t.hits == 1);
    CHECK_NEAR(t.face.cx, 0.8f, 1e-6f);
}

void test_tracker_filters_weak_and_tiny_boxes()
{
    FaceTracker tracker;
    auto t = tracker.update({face(0.0f, 0.0f, 0.2f, 0.3f)}, 0);  // low score
    CHECK(!t.present);
    t = tracker.update({face(0.0f, 0.0f, 0.01f, 0.95f)}, 100);  // 3 px wide on a 320 px image
    CHECK(!t.present);
}

void test_yuyv_to_rgb565()
{
    // 4 x 2 YUYV image: gray, white, black, saturated red
    const uint8_t gray[]  = {128, 128, 128, 128};
    uint16_t out[2]       = {0, 0};
    pixel::yuyvToRgb565(gray, 2, 1, 1, out);
    CHECK(out[0] == 0x8410);
    CHECK(out[1] == 0x8410);

    const uint8_t white_black[] = {255, 128, 0, 128};
    pixel::yuyvToRgb565(white_black, 2, 1, 1, out);
    CHECK(out[0] == 0xFFFF);
    CHECK(out[1] == 0x0000);

    // Full-range BT.601 red: Y 76, Cb 85, Cr 255
    const uint8_t red[] = {76, 85, 76, 255};
    pixel::yuyvToRgb565(red, 2, 1, 1, out);
    CHECK((out[0] >> 11) >= 30);           // R5
    CHECK(((out[0] >> 5) & 0x3F) <= 3);    // G6
    CHECK((out[0] & 0x1F) <= 2);           // B5

    // Same as esp-dl / the single pixel helper
    CHECK(pixel::yuvToRgb565(128, 128, 128) == 0x8410);
    CHECK(pixel::yuvToRgb565(76, 85, 255) == out[0]);
}

void test_yuyv_downscale()
{
    // 4 x 2 image -> 2 x 1: each output pixel averages a 2 x 2 block (luma) and the two rows' chroma
    const uint8_t src[] = {
        0,   128, 40,  128, 200, 128, 200, 128,  // row 0
        80,  128, 120, 128, 200, 128, 200, 128,  // row 1
    };
    uint16_t out[3] = {0, 0, 0xBEEF};
    pixel::yuyvToRgb565(src, 4, 2, 2, out);
    CHECK(out[0] == pixel::yuvToRgb565(60, 128, 128));
    CHECK(out[1] == pixel::yuvToRgb565(200, 128, 128));
    CHECK(out[2] == 0xBEEF);  // exactly width/2 * height/2 values written
}

void test_class_probability()
{
    // Stage 2 logits as the CoreS3 logged them for a ceiling crop: 2.12 (background), -1.88 (face) -> face 0.018
    const int8_t ceiling[2] = {17, -15};  // x 0.125 = 2.125, -1.875
    CHECK_NEAR(classProbability(ceiling, 2, 0.125f, 1), 0.018f, 0.001f);
    CHECK_NEAR(classProbability(ceiling, 2, 0.125f, 0), 0.982f, 0.001f);
    const int8_t face_like[2] = {-24, 24};  // -3, 3
    CHECK_NEAR(classProbability(face_like, 2, 0.125f, 1), 0.9975f, 0.0005f);
    // The probability of the asked class, not of the winning one (the esp-dl MNPPostprocessor bug)
    CHECK(classProbability(ceiling, 2, 0.125f, 1) < 0.5f);
    const int8_t tie[2] = {5, 5};
    CHECK_NEAR(classProbability(tie, 2, 1.0f, 1), 0.5f, 1e-6f);
    // Classes sum to 1
    const int16_t three[3] = {100, -50, 7};
    float sum = 0.0f;
    for (int c = 0; c < 3; c++) {
        sum += classProbability(three, 3, 0.01f, c);
    }
    CHECK_NEAR(sum, 1.0f, 1e-5f);
    // One class: a sigmoid logit
    const int8_t one[1] = {0};
    CHECK_NEAR(classProbability(one, 1, 1.0f, 0), 0.5f, 1e-6f);
    const int8_t one_high[1] = {16};
    CHECK_NEAR(classProbability(one_high, 1, 0.25f, 0), 1.0f / (1.0f + std::exp(-4.0f)), 1e-5f);
    // Extremes stay finite
    const int8_t extreme8[2]   = {127, -128};
    const int16_t extreme16[2] = {-32768, 32767};
    const float low            = classProbability(extreme8, 2, 1.0f, 1);
    const float high           = classProbability(extreme16, 2, 1.0f, 1);
    CHECK(std::isfinite(low) && low >= 0.0f && low < 1e-6f);
    CHECK(std::isfinite(high) && high > 0.999999f && high <= 1.0f);
    const int8_t sigmoid_low[1] = {-128};
    CHECK(std::isfinite(classProbability(sigmoid_low, 1, 1.0f, 0)));
}

void test_above_threshold()
{
    CHECK(aboveThreshold(0.51f, 0.5f));
    CHECK(!aboveThreshold(0.5f, 0.5f));  // equal is not enough, like esp-dl
    CHECK(!aboveThreshold(0.018f, 0.5f));
}

void test_internal_budget()
{
    constexpr size_t K = 1024;
    InternalHeap radar{138 * K, 67 * K, 31 * K};  // measured in the RADAR (no Wi-Fi)
    CHECK(fitInternalBudget(32 * K, radar) == 27 * K);  // the largest SIMD block minus the slack
    CHECK(fitInternalBudget(0, radar) == 0);
    CHECK(fitInternalBudget(16 * K, radar) == 16 * K);  // fits as asked

    InternalHeap remote{99 * K, 29 * K, 28 * K};  // measured in the REMOTE (Wi-Fi on): no SIMD RAM to spare
    CHECK(fitInternalBudget(32 * K, remote) == 0);

    InternalHeap low_total{60 * K, 200 * K, 100 * K};  // only 12 KB above the internal reserve
    CHECK(fitInternalBudget(64 * K, low_total) == 12 * K);
    InternalHeap tiny{200 * K, 200 * K, 11 * K};  // a 7 KB budget is not worth it
    CHECK(fitInternalBudget(64 * K, tiny) == 0);
    InternalHeap empty{0, 0, 0};  // no unsigned wrap-around
    CHECK(fitInternalBudget(64 * K, empty) == 0);
    InternalHeap reserves_only{48 * K, 32 * K, 4 * K};
    CHECK(fitInternalBudget(64 * K, reserves_only) == 0);
}

void test_boxes_to_faces()
{
    std::vector<Face> out;
    // Normalization: the whole 320x240 frame is centred, full size (corners clipped to the last pixel)
    boxesToFaces({FaceBox{-20, -10, 400, 300, 0.9f}}, 320, 240, 4, out);
    CHECK(out.size() == 1);
    CHECK_NEAR(out[0].cx, (0.0f + 319.0f) / 320.0f - 1.0f, 1e-6f);
    CHECK_NEAR(out[0].cy, (0.0f + 239.0f) / 240.0f - 1.0f, 1e-6f);
    CHECK_NEAR(out[0].w, 319.0f / 320.0f, 1e-6f);
    CHECK_NEAR(out[0].h, 239.0f / 240.0f, 1e-6f);
    // A face in the right half, lower part
    boxesToFaces({FaceBox{240, 120, 280, 180, 0.7f}}, 320, 240, 4, out);
    CHECK(out.size() == 1);
    CHECK_NEAR(out[0].cx, 0.625f, 1e-6f);
    CHECK_NEAR(out[0].cy, 0.25f, 1e-6f);
    CHECK_NEAR(out[0].w, 0.125f, 1e-6f);
    CHECK_NEAR(out[0].score, 0.7f, 1e-6f);
    // Empty boxes (also after the clipping) are dropped
    boxesToFaces({FaceBox{50, 50, 50, 80, 0.9f}, FaceBox{10, 60, 40, 60, 0.9f}, FaceBox{330, 10, 400, 50, 0.9f},
                  FaceBox{80, 40, 60, 90, 0.9f}},
                 320, 240, 4, out);
    CHECK(out.empty());
    // Best first, at most maxFaces
    std::vector<FaceBox> many;
    for (int i = 0; i < 6; i++) {
        many.push_back(FaceBox{10 * i, 10, 10 * i + 30, 50, 0.5f + 0.05f * static_cast<float>((i * 7) % 6)});
    }
    boxesToFaces(many, 320, 240, 4, out);
    CHECK(out.size() == 4);
    for (size_t i = 1; i < out.size(); i++) {
        CHECK(out[i - 1].score >= out[i].score);
    }
    CHECK_NEAR(out[0].score, 0.75f, 1e-6f);
    boxesToFaces(many, 0, 240, 4, out);  // no image: nothing
    CHECK(out.empty());
}

void test_mean_luma()
{
    std::vector<uint16_t> image(320 * 240, 0x8410);
    CHECK_NEAR(meanLumaRgb565(image.data(), 320, 240), 130.0f, 3.0f);
    std::vector<uint16_t> dark(320 * 240, 0x0000);
    CHECK(meanLumaRgb565(dark.data(), 320, 240) == 0.0f);
    CHECK(meanLumaRgb565(nullptr, 320, 240) == 0.0f);
}

void test_radar_sfx()
{
    const auto& alarm = synth::sfxTones(Sfx::Alarm);
    CHECK(synth::sequenceDurationMs(alarm) == 3000);  // "警报声持续 3 秒"
    // High-low two-tone: alternating pitches
    for (size_t i = 1; i < alarm.size(); i++) {
        CHECK(alarm[i].freqHz != alarm[i - 1].freqHz);
        CHECK(alarm[i].freqHz > 0);
        CHECK(alarm[i].volume <= 100);
    }

    const auto& ping = synth::sfxTones(Sfx::RadarPing);
    const uint32_t ping_ms = synth::sequenceDurationMs(ping);
    CHECK(ping_ms >= 300 && ping_ms <= 900);  // well inside the 1.8 s ping period
    // A sine ping followed by fading echoes of the same pitch
    int last_volume = 101;
    int notes       = 0;
    for (const auto& t : ping) {
        if (t.freqHz == 0) {
            continue;
        }
        CHECK(t.wave == Wave::Sine);
        CHECK(t.volume < last_volume);
        CHECK(t.freqHz == ping.front().freqHz);
        last_volume = t.volume;
        notes++;
    }
    CHECK(notes >= 2);

    // Renders without clipping
    synth::ToneRenderer renderer(24000);
    for (const auto& t : alarm) {
        renderer.begin(t);
        int16_t buf[240];
        size_t n = 0;
        while ((n = renderer.render(buf, 240)) > 0) {
            for (size_t i = 0; i < n; i++) {
                CHECK(std::abs(buf[i]) <= synth::ToneRenderer::peakLevel(t.wave));
            }
        }
    }
}

}  // namespace

int main()
{
    RUN_TEST(test_tracker_hits_and_smoothing);
    RUN_TEST(test_tracker_keeps_its_target);
    RUN_TEST(test_tracker_miss_breaks_the_streak);
    RUN_TEST(test_tracker_drops_a_lost_target);
    RUN_TEST(test_tracker_gate_and_new_track);
    RUN_TEST(test_tracker_filters_weak_and_tiny_boxes);
    RUN_TEST(test_yuyv_to_rgb565);
    RUN_TEST(test_yuyv_downscale);
    RUN_TEST(test_class_probability);
    RUN_TEST(test_above_threshold);
    RUN_TEST(test_internal_budget);
    RUN_TEST(test_boxes_to_faces);
    RUN_TEST(test_mean_luma);
    RUN_TEST(test_radar_sfx);
    std::cout << "all radar vision tests passed (" << stackpet_test::checks() << " checks)\n";
    return 0;
}
