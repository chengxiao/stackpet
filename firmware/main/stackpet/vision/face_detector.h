/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stackpet/camera/frame_grabber.h>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace stackpet::vision {

// One face box in normalized image coordinates (as captured: +x = right, +y = down).
struct Face {
    float cx    = 0.0f;  // box centre, -1 .. 1
    float cy    = 0.0f;
    float w     = 0.0f;  // box size as a fraction of the image width / height
    float h     = 0.0f;
    float score = 0.0f;  // 0 .. 1
};

// The result of one detector run.
struct FaceFrame {
    std::vector<Face> faces;  // best score first
    RgbFrame frame;           // the analyzed camera frame (the RADAR lock view shows it with the boxes)
    uint32_t seq     = 0;     // increments for every detector run
    float inferMs    = 0.0f;  // time spent in FaceModel::run()
    float meanLuma   = 0.0f;  // rough brightness of the frame, 0-255 (warn when the room is too dark)
};

struct TrackedFace {
    bool present        = false;  // a face is being tracked (matched within the hold limits)
    int hits            = 0;      // consecutive detector runs that found it
    int misses          = 0;      // consecutive runs without it since the last hit
    Face face;                    // smoothed box (display)
    Face raw;                     // the last matched detection as is (aiming: no smoothing lag)
    uint32_t lastSeenMs = 0;      // capture time of the last match
};

struct FaceTrackerConfig {
    float gate      = 0.45f;  // max centre distance (normalized units) to continue the track
    float smoothing = 0.6f;   // weight of a new detection in the average (1 = no smoothing)
    int maxMisses   = 2;
    uint32_t holdMs = 800;
    float minScore  = 0.5f;
    float minSize   = 0.03f;  // ignore boxes narrower than this fraction of the image (noise)
};

/**
 * @brief Pure C++ temporal filter for the detector output (host tested, no ESP-IDF deps).
 *
 * Follows one target: a detection close to the tracked face (gate) continues the track and is blended in with an
 * exponential moving average, otherwise the best scoring detection starts a new track. The track survives
 * `maxMisses` runs (or `holdMs`) without a match, so a single missed frame does not reset the hit streak count
 * that the RADAR uses to confirm a target (it only counts consecutive hits).
 */
class FaceTracker {
public:
    using Config = FaceTrackerConfig;

    explicit FaceTracker(const Config& config = Config());

    void reset();
    // Feed one detector run (faces of the frame captured at nowMs, any order).
    const TrackedFace& update(const std::vector<Face>& faces, uint32_t nowMs);
    const TrackedFace& target() const;

private:
    Config _config;
    TrackedFace _target;
};

// Rough brightness of an RGB565 image from a sparse grid of samples (0 for an empty image).
float meanLumaRgb565(const uint16_t* pixels, int width, int height);

// A detector box in image pixels (corners as esp-dl reports them)
struct FaceBox {
    int x0      = 0;
    int y0      = 0;
    int x1      = 0;
    int y1      = 0;
    float score = 0.0f;
};

// Detector boxes -> normalized faces: corners clipped to the image, empty boxes dropped, best score first, at most
// maxFaces. Pure C++ (host tested).
void boxesToFaces(const std::vector<FaceBox>& boxes, int width, int height, size_t maxFaces, std::vector<Face>& out);

struct FaceDetectorStats {
    bool ready         = false;  // model loaded, detecting
    bool failed        = false;  // the model could not be loaded: use the motion fallback
    uint32_t runs      = 0;
    float fps          = 0.0f;   // detector runs per second (smoothed)
    float inferMs      = 0.0f;   // average FaceModel::run() time
    float maxInferMs   = 0.0f;   // worst run of the last log period
    float loadMs       = 0.0f;   // model load time
    size_t psramUsed   = 0;      // PSRAM taken by the loaded model
    size_t internalUsed = 0;     // internal RAM taken by the loaded model
};

/**
 * @brief The human_face_detect MSR+MNP models (int8, see FaceModel) running in their own task on core 0.
 *
 * The task pulls RGB565 frames from the FrameGrabber (requesting the next one before it starts on the current
 * one, so the conversion on core 1 overlaps the inference), runs the two-stage detector and publishes the
 * faces together with the analyzed frame. It runs at the priority of the mooncake main loop (1) so the UI and
 * the servos are never starved, and it blocks for one tick after every run so IDLE0 (task watchdog) runs too.
 *
 * The model is loaded when start() is called and freed by stop(). The task itself is created once and parks
 * between start/stop cycles (its stack lives in PSRAM). All methods are thread-safe. ESP-IDF only.
 */
class FaceDetector {
public:
    static FaceDetector& instance();

    // Start detecting (the model loads asynchronously, see stats().ready / failed). Returns false only when the
    // task cannot be created. The FrameGrabber must be running.
    bool start();
    // Stop detecting and free the model; waits for the current run to finish (bounded).
    void stop();
    bool isRunning() const;

    // Copy the latest result if it is newer than afterSeq.
    bool latest(uint32_t afterSeq, FaceFrame& out);
    FaceDetectorStats stats() const;
};

}  // namespace stackpet::vision
