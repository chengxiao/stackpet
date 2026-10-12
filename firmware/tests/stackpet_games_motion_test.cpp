/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "stackpet_games_check.h"
#include <stackpet/vision/motion_detector.h>
#include <cstdint>
#include <vector>

using stackpet::vision::MotionDetector;
using stackpet::vision::MotionResult;

namespace {

constexpr int W = 40;
constexpr int H = 30;

// Textured, deterministic background (like a real room, not a flat wall)
std::vector<uint8_t> makeScene(uint32_t seed = 7)
{
    std::vector<uint8_t> frame(W * H);
    uint32_t x = seed;
    for (int i = 0; i < W * H; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        frame[i] = static_cast<uint8_t>(60 + (x % 100));
    }
    return frame;
}

void paintRect(std::vector<uint8_t>& frame, int x0, int y0, int w, int h, uint8_t value)
{
    for (int y = y0; y < y0 + h; y++) {
        for (int x = x0; x < x0 + w; x++) {
            frame[y * W + x] = value;
        }
    }
}

void testNoReferenceIsInvalid()
{
    MotionDetector detector;
    auto frame = makeScene();
    CHECK(!detector.hasReference());
    CHECK(!detector.compare(frame.data(), W, H).valid);

    detector.setReference(frame.data(), W, H);
    CHECK(detector.hasReference());
    CHECK(!detector.compare(frame.data(), W + 1, H).valid);  // size mismatch
    CHECK(!detector.compare(nullptr, W, H).valid);

    detector.clearReference();
    CHECK(!detector.hasReference());
}

void testStaticSceneHasNoMotion()
{
    MotionDetector detector;
    auto frame = makeScene();
    detector.setReference(frame.data(), W, H);

    // Small sensor noise (+-3) everywhere
    auto noisy = frame;
    for (int i = 0; i < W * H; i++) {
        noisy[i] = static_cast<uint8_t>(noisy[i] + (i % 7) - 3);
    }
    auto result = detector.compare(noisy.data(), W, H);
    CHECK(result.valid);
    CHECK(result.changedCells == 0);
    CHECK_NEAR(result.score, 0.0f, 1e-6f);
}

void testExposureChangeIsCompensated()
{
    MotionDetector detector;
    auto frame = makeScene();
    detector.setReference(frame.data(), W, H);

    // Auto exposure brightens the whole frame by 25%
    auto brighter = frame;
    for (auto& v : brighter) {
        v = static_cast<uint8_t>(std::min(255, static_cast<int>(v * 1.25f)));
    }
    auto result = detector.compare(brighter.data(), W, H);
    CHECK(result.valid);
    CHECK(result.meanShift > 10.0f);
    CHECK(result.score < 0.01f);

    // Pure offset shift (dark scene behaviour)
    auto shifted = frame;
    for (auto& v : shifted) {
        v = static_cast<uint8_t>(v + 30);
    }
    result = detector.compare(shifted.data(), W, H);
    CHECK(result.valid);
    CHECK_NEAR(result.meanShift, 30.0f, 0.01f);
    CHECK(result.score < 0.01f);
}

void testLocalMotionCentroid()
{
    MotionDetector detector;
    auto frame = makeScene();
    detector.setReference(frame.data(), W, H);

    // A "person" (dark block) appears on the right side, upper half
    auto moved = frame;
    paintRect(moved, 30, 4, 6, 8, 5);
    auto result = detector.compare(moved.data(), W, H);
    CHECK(result.valid);
    CHECK(result.changedCells > 20);
    CHECK(result.score > 0.02f);
    CHECK(result.cx > 0.4f);   // right
    CHECK(result.cy < -0.2f);  // top

    // Same block on the lower left
    moved = frame;
    paintRect(moved, 2, 20, 6, 8, 250);
    result = detector.compare(moved.data(), W, H);
    CHECK(result.cx < -0.4f);
    CHECK(result.cy > 0.2f);
}

void testIsolatedCellIsIgnored()
{
    MotionDetector detector;
    auto frame = makeScene();
    detector.setReference(frame.data(), W, H);

    auto moved          = frame;
    moved[10 * W + 10]  = 255;  // single hot pixel
    moved[20 * W + 30]  = 0;
    auto result = detector.compare(moved.data(), W, H);
    CHECK(result.changedCells == 0);

    // Two adjacent cells form a (tiny) object and are kept
    moved[10 * W + 11] = 255;
    moved[10 * W + 10] = 255;
    result             = detector.compare(moved.data(), W, H);
    CHECK(result.changedCells == 2);
}

void testCellThreshold()
{
    MotionDetector detector;
    CHECK(detector.cellThreshold() == 24);
    auto frame = makeScene();
    detector.setReference(frame.data(), W, H);

    auto moved = frame;
    for (int y = 10; y < 14; y++) {
        for (int x = 10; x < 14; x++) {
            moved[y * W + x] = static_cast<uint8_t>(frame[y * W + x] + 20);
        }
    }
    CHECK(detector.compare(moved.data(), W, H).changedCells == 0);  // 20 < default threshold 24

    detector.setCellThreshold(10);
    CHECK(detector.compare(moved.data(), W, H).changedCells == 16);
}

void testLargestBlobPicksTheBigMover()
{
    MotionDetector detector;
    auto frame = makeScene();
    detector.setReference(frame.data(), W, H);

    // Big mover on the right, small mover on the left: the plain centroid lands in between
    auto moved = frame;
    paintRect(moved, 30, 10, 8, 10, 5);  // 80 cells
    paintRect(moved, 2, 12, 4, 5, 5);    // 20 cells
    auto result = detector.compare(moved.data(), W, H);
    CHECK(result.changedCells == 100);
    CHECK(result.blobCells == 80);
    CHECK(result.blobCx > 0.6f);
    CHECK(result.cx < result.blobCx - 0.2f);
    CHECK(result.blobCy > -0.2f && result.blobCy < 0.3f);

    // Nothing moved: no blob
    result = detector.compare(frame.data(), W, H);
    CHECK(result.blobCells == 0);
}

void testMeanLuma()
{
    std::vector<uint8_t> dark(W * H, 10);
    std::vector<uint8_t> bright(W * H, 200);
    CHECK_NEAR(stackpet::vision::meanLuma(dark.data(), W, H), 10.0f, 1e-4f);
    CHECK_NEAR(stackpet::vision::meanLuma(bright.data(), W, H), 200.0f, 1e-4f);
    CHECK(stackpet::vision::meanLuma(nullptr, W, H) == 0.0f);
    CHECK(stackpet::vision::meanLuma(dark.data(), 0, H) == 0.0f);
}

}  // namespace

int main()
{
    RUN_TEST(testNoReferenceIsInvalid);
    RUN_TEST(testStaticSceneHasNoMotion);
    RUN_TEST(testExposureChangeIsCompensated);
    RUN_TEST(testLocalMotionCentroid);
    RUN_TEST(testIsolatedCellIsIgnored);
    RUN_TEST(testCellThreshold);
    RUN_TEST(testLargestBlobPicksTheBigMover);
    RUN_TEST(testMeanLuma);
    std::cout << stackpet_test::checks() << " checks passed\n";
    return 0;
}
