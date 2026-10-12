/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "face_detector.h"
#include <algorithm>
#include <cmath>

namespace stackpet::vision {

/* -------------------------------------------------------------------------- */
/*                         FaceTracker (pure C++ part)                        */
/* -------------------------------------------------------------------------- */
FaceTracker::FaceTracker(const Config& config) : _config(config)
{
}

void FaceTracker::reset()
{
    _target = TrackedFace{};
}

const TrackedFace& FaceTracker::update(const std::vector<Face>& faces, uint32_t nowMs)
{
    const Face* best_score = nullptr;
    const Face* best_match = nullptr;
    float best_distance    = _config.gate;
    for (const auto& face : faces) {
        if (face.score < _config.minScore || face.w < _config.minSize) {
            continue;
        }
        if (best_score == nullptr || face.score > best_score->score) {
            best_score = &face;
        }
        if (_target.present) {
            float distance = std::hypot(face.cx - _target.face.cx, face.cy - _target.face.cy);
            if (distance <= best_distance) {
                best_distance = distance;
                best_match    = &face;
            }
        }
    }

    if (best_match != nullptr) {
        const float a = std::clamp(_config.smoothing, 0.0f, 1.0f);
        _target.face.cx += a * (best_match->cx - _target.face.cx);
        _target.face.cy += a * (best_match->cy - _target.face.cy);
        _target.face.w += a * (best_match->w - _target.face.w);
        _target.face.h += a * (best_match->h - _target.face.h);
        _target.face.score = best_match->score;
        _target.raw        = *best_match;
        _target.hits++;
        _target.misses     = 0;
        _target.lastSeenMs = nowMs;
        return _target;
    }

    if (_target.present) {
        // Missed: the streak is broken, the track itself survives a short gap
        _target.hits = 0;
        _target.misses++;
        const bool too_old = static_cast<int32_t>(nowMs - _target.lastSeenMs) > static_cast<int32_t>(_config.holdMs);
        if (_target.misses > _config.maxMisses || too_old) {
            _target = TrackedFace{};
        }
    }

    if (!_target.present && best_score != nullptr) {
        _target.present    = true;
        _target.hits       = 1;
        _target.misses     = 0;
        _target.face       = *best_score;
        _target.raw        = *best_score;
        _target.lastSeenMs = nowMs;
    }
    return _target;
}

const TrackedFace& FaceTracker::target() const
{
    return _target;
}

float meanLumaRgb565(const uint16_t* pixels, int width, int height)
{
    if (pixels == nullptr || width <= 0 || height <= 0) {
        return 0.0f;
    }
    constexpr int kStep = 8;
    uint32_t sum        = 0;
    uint32_t count      = 0;
    for (int y = kStep / 2; y < height; y += kStep) {
        const uint16_t* row = pixels + static_cast<size_t>(y) * width;
        for (int x = kStep / 2; x < width; x += kStep) {
            uint16_t px = row[x];
            uint32_t r  = (px >> 11) << 3;
            uint32_t g  = ((px >> 5) & 0x3F) << 2;
            uint32_t b  = (px & 0x1F) << 3;
            sum += (77 * r + 150 * g + 29 * b) >> 8;
            count++;
        }
    }
    return count > 0 ? static_cast<float>(sum) / static_cast<float>(count) : 0.0f;
}

void boxesToFaces(const std::vector<FaceBox>& boxes, int width, int height, size_t maxFaces, std::vector<Face>& out)
{
    out.clear();
    if (width <= 0 || height <= 0) {
        return;
    }
    const float inv_w = 1.0f / static_cast<float>(width);
    const float inv_h = 1.0f / static_cast<float>(height);
    for (const auto& box : boxes) {
        const float x0 = static_cast<float>(std::clamp(box.x0, 0, width - 1));
        const float y0 = static_cast<float>(std::clamp(box.y0, 0, height - 1));
        const float x1 = static_cast<float>(std::clamp(box.x1, 0, width - 1));
        const float y1 = static_cast<float>(std::clamp(box.y1, 0, height - 1));
        if (x1 <= x0 || y1 <= y0) {
            continue;
        }
        Face face;
        face.cx    = (x0 + x1) * inv_w - 1.0f;
        face.cy    = (y0 + y1) * inv_h - 1.0f;
        face.w     = (x1 - x0) * inv_w;
        face.h     = (y1 - y0) * inv_h;
        face.score = box.score;
        out.push_back(face);
    }
    std::stable_sort(out.begin(), out.end(), [](const Face& a, const Face& b) { return a.score > b.score; });
    if (out.size() > maxFaces) {
        out.resize(maxFaces);
    }
}

}  // namespace stackpet::vision

/* -------------------------------------------------------------------------- */
/*                       FaceDetector (ESP-IDF task part)                     */
/* -------------------------------------------------------------------------- */
#ifdef ESP_PLATFORM
#include "face_model.h"
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mooncake_log.h>
#include <fmt/format.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <new>

namespace stackpet::vision {

namespace {

const char* kTag = "FaceDetector";

// A PSRAM stack costs nothing measurable (243 vs 244 ms per run with the stack in internal RAM) and keeps ~16 KB of
// internal RAM free. The task never writes flash, which is the one thing a PSRAM stack must not do.
constexpr uint32_t kTaskStackSize   = 16 * 1024;
constexpr UBaseType_t kTaskPriority = 1;  // = the mooncake main loop (app_main): round-robin, never above it
constexpr BaseType_t kTaskCore      = 0;  // core 1 has LVGL, the camera capture and the sound player
constexpr uint32_t kFrameWaitMs     = 200;
constexpr uint32_t kStopTimeoutMs   = 3000;
constexpr int64_t kLogPeriodUs      = 5000000;
constexpr int64_t kFpsWindowUs      = 1000000;
constexpr size_t kMinFreePsram      = 1024 * 1024;
constexpr uint32_t kLoggedFirstRuns = 3;
constexpr size_t kMaxFaces          = 4;
constexpr float kEmaAlpha           = 0.2f;

// Measured on the CoreS3 in the RADAR (no Wi-Fi): stage 1 (MSR) takes 81 ms with its feature maps in PSRAM and 71 ms
// with 27 KB of them in internal RAM (the largest SIMD-capable block there); stage 2 (MNP) 12.6 ms per candidate.
// The RADAR follows one target: three candidates keep a group of people from slowing the detector down.
constexpr size_t kMsrInternalBytes = 32 * 1024;
constexpr int kMaxCandidates       = 3;

struct State {
    std::mutex mutex;
    std::condition_variable cv;
    bool task_created = false;
    bool active       = false;  // inside the detect loop (loading, detecting or unloading the model)
    std::atomic<bool> run{false};
    uint32_t stops = 0;  // incremented by every stop(): wakes a task parked after a failed model load
    FaceFrame latest;
    FaceDetectorStats stats;
    uint32_t seq = 0;
};

// Free heap right before the model was loaded, to report what an unload gives back
struct HeapMark {
    size_t psram    = 0;
    size_t internal = 0;
};

State& state()
{
    static State instance;
    return instance;
}

float ema(float current, float sample)
{
    return current <= 0.0f ? sample : current + kEmaAlpha * (sample - current);
}

void set_failed(const char* reason)
{
    mclog::tagError(kTag, "face detection unavailable: {}", reason);
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.stats.failed = true;
    s.stats.ready  = false;
}

std::unique_ptr<FaceModel> load_model(HeapMark& before)
{
    auto& s                     = state();
    const size_t psram_before    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    before.psram                 = psram_before;
    before.internal              = internal_before;
    if (psram_before < kMinFreePsram) {
        set_failed("not enough PSRAM");
        return nullptr;
    }

    const int64_t start = esp_timer_get_time();
    std::unique_ptr<FaceModel> detect;
    try {
        FaceModelConfig config;
        config.msrInternalBytes = kMsrInternalBytes;
        config.maxCandidates    = kMaxCandidates;
        detect                  = std::make_unique<FaceModel>(config);
    } catch (const std::exception&) {
        // std::bad_alloc in practice (a model or tensor allocation): the RADAR falls back to motion detection
        detect.reset();
    }
    if (!detect) {
        set_failed("model allocation failed");
        return nullptr;
    }

    const float load_ms          = (esp_timer_get_time() - start) / 1000.0f;
    const size_t psram_after     = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t internal_after  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t psram_used      = psram_before > psram_after ? psram_before - psram_after : 0;
    const size_t internal_used   = internal_before > internal_after ? internal_before - internal_after : 0;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.stats.ready        = true;
        s.stats.failed       = false;
        s.stats.loadMs       = load_ms;
        s.stats.psramUsed    = psram_used;
        s.stats.internalUsed = internal_used;
    }
    mclog::tagInfo(kTag,
                   "MSR+MNP loaded in {:.0f} ms: {} KB PSRAM, {} KB internal (feature maps {} KB / {} KB), free now "
                   "{} KB / {} KB, D/IRAM {} KB (largest {} KB)",
                   load_ms, psram_used / 1024, internal_used / 1024, detect->msrInternalBytes() / 1024,
                   detect->mnpInternalBytes() / 1024, psram_after / 1024, internal_after / 1024,
                   heap_caps_get_free_size(kFaceModelInternalCaps) / 1024,
                   heap_caps_get_largest_free_block(kFaceModelInternalCaps) / 1024);
    return detect;
}

void to_faces(const std::list<dl::detect::result_t>& results, int width, int height, std::vector<FaceBox>& boxes,
              std::vector<Face>& out)
{
    boxes.clear();
    for (const auto& r : results) {
        if (r.box.size() >= 4) {
            boxes.push_back(FaceBox{r.box[0], r.box[1], r.box[2], r.box[3], r.score});
        }
    }
    boxesToFaces(boxes, width, height, kMaxFaces, out);
}

// Returns false if the model could not be loaded
bool detect_loop()
{
    auto& s = state();
    HeapMark before;
    auto detect = load_model(before);
    if (!detect) {
        return false;
    }

    auto& grabber          = FrameGrabber::instance();
    uint32_t frame_seq     = 0;
    uint32_t runs          = 0;
    uint32_t window_runs   = 0;
    int64_t window_start   = esp_timer_get_time();
    int64_t last_log       = window_start;
    float infer_avg        = 0.0f;
    float infer_max        = 0.0f;
    float msr_avg          = 0.0f;
    float mnp_avg          = 0.0f;
    float candidates_avg   = 0.0f;
    float fps              = 0.0f;
    float age_avg          = 0.0f;
    std::vector<FaceBox> boxes;
    std::vector<Face> faces;

    while (s.run.load()) {
        RgbFrame frame;
        if (!grabber.waitRgb(frame_seq, frame, kFrameWaitMs)) {
            continue;
        }
        frame_seq = frame.seq;
        if (!frame.pixels || frame.width == 0 || frame.height == 0) {
            continue;
        }
        // Have the next frame converted on core 1 while this one is analyzed
        grabber.requestRgb();

        dl::image::img_t img;
        img.data     = const_cast<uint16_t*>(frame.pixels.get());
        img.width    = frame.width;
        img.height   = frame.height;
        img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE;

        const int64_t start = esp_timer_get_time();
        FaceModelTiming timing;
        FaceFrame out;
        try {
            to_faces(detect->run(img, &timing), frame.width, frame.height, boxes, faces);
            out.faces = faces;
        } catch (const std::exception& e) {
            // Out of memory for the result lists, most likely: skip this frame rather than abort
            mclog::tagWarn(kTag, "detection failed: {}", e.what());
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        const int64_t end = esp_timer_get_time();
        const float infer = (end - start) / 1000.0f;

        out.inferMs  = infer;
        out.meanLuma = meanLumaRgb565(frame.pixels.get(), frame.width, frame.height);
        out.frame    = std::move(frame);

        runs++;
        window_runs++;
        infer_avg      = ema(infer_avg, infer);
        infer_max      = std::max(infer_max, infer);
        msr_avg        = ema(msr_avg, timing.msrMs);
        mnp_avg        = ema(mnp_avg, timing.mnpMs);
        candidates_avg = ema(candidates_avg, static_cast<float>(timing.candidates));
        age_avg   = ema(age_avg, (end - out.frame.timestampUs) / 1000.0f);
        if (end - window_start >= kFpsWindowUs) {
            fps          = window_runs * 1000000.0f / static_cast<float>(end - window_start);
            window_runs  = 0;
            window_start = end;
        }

        {
            std::lock_guard<std::mutex> lock(s.mutex);
            out.seq            = ++s.seq;
            s.latest           = std::move(out);
            s.stats.runs       = runs;
            s.stats.fps        = fps;
            s.stats.inferMs    = infer_avg;
            s.stats.maxInferMs = std::max(s.stats.maxInferMs, infer);
        }

        if (runs <= kLoggedFirstRuns) {
            mclog::tagInfo(kTag, "run {}: {:.1f} ms, {} face(s)", runs, infer, faces.size());
        }
        if (end - last_log >= kLogPeriodUs) {
            last_log = end;
            mclog::tagInfo(kTag,
                           "{} runs, {:.1f} fps, infer {:.1f} ms (max {:.1f}) = msr {:.1f} + mnp {:.1f} ({:.1f} "
                           "candidates), result age {:.0f} ms, {} face(s){}",
                           runs, fps, infer_avg, infer_max, msr_avg, mnp_avg, candidates_avg, age_avg, faces.size(),
                           faces.empty() ? "" : fmt::format(", best {:.2f}", faces.front().score));
            infer_max = 0.0f;
            std::lock_guard<std::mutex> lock(s.mutex);
            s.stats.maxInferMs = 0.0f;
        }

        // One tick off the CPU: IDLE0 must run for the task watchdog, the main loop gets a free slot as well
        vTaskDelay(1);
    }

    // Drop our own frame references first, so the numbers below are about the model only (frames still held by
    // the app, 150 KB of PSRAM each, show up as PSRAM not given back)
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.latest = FaceFrame{};
    }
    detect.reset();
    const size_t psram_after    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t internal_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    mclog::tagInfo(kTag, "stopped after {} runs, model freed: PSRAM free {} KB ({:+d} KB vs before the load), "
                         "internal free {} KB ({:+d} KB)",
                   runs, psram_after / 1024,
                   static_cast<int>((static_cast<int64_t>(psram_after) - static_cast<int64_t>(before.psram)) / 1024),
                   internal_after / 1024,
                   static_cast<int>((static_cast<int64_t>(internal_after) - static_cast<int64_t>(before.internal)) / 1024));
    return true;
}

void task_main(void* /*arg*/)
{
    auto& s = state();
    while (true) {
        {
            std::unique_lock<std::mutex> lock(s.mutex);
            s.cv.wait(lock, [&]() { return s.run.load(); });
            s.active = true;
        }

        const bool loaded = detect_loop();

        std::unique_lock<std::mutex> lock(s.mutex);
        s.active      = false;
        s.stats.ready = false;
        s.latest      = FaceFrame{};  // release the last frame buffer
        s.cv.notify_all();
        if (!loaded) {
            // A failed load returns at once: park until the next stop(), do not retry in a loop
            const uint32_t stops = s.stops;
            s.cv.wait(lock, [&]() { return s.stops != stops; });
        }
    }
}

bool ensure_task()
{
    auto& s = state();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.task_created) {
            return true;
        }
    }

    auto* stack = static_cast<StackType_t*>(heap_caps_malloc(kTaskStackSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto* tcb   = static_cast<StaticTask_t*>(heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL));
    if (stack == nullptr || tcb == nullptr) {
        heap_caps_free(stack);
        heap_caps_free(tcb);
        mclog::tagError(kTag, "failed to allocate the detector task");
        return false;
    }
    // Never deleted (it parks between start/stop), so the static stack and TCB stay valid for the whole uptime
    if (xTaskCreateStaticPinnedToCore(task_main, "face_detect", kTaskStackSize, nullptr, kTaskPriority, stack, tcb,
                                      kTaskCore) == nullptr) {
        heap_caps_free(stack);
        heap_caps_free(tcb);
        mclog::tagError(kTag, "failed to create the detector task");
        return false;
    }
    std::lock_guard<std::mutex> lock(s.mutex);
    s.task_created = true;
    return true;
}

}  // namespace

FaceDetector& FaceDetector::instance()
{
    static FaceDetector detector;
    return detector;
}

bool FaceDetector::start()
{
    if (!ensure_task()) {
        std::lock_guard<std::mutex> lock(state().mutex);
        state().stats.failed = true;
        return false;
    }
    auto& s = state();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.run.load()) {
            return true;
        }
        s.stats = FaceDetectorStats{};
        s.run.store(true);
    }
    s.cv.notify_all();
    mclog::tagInfo(kTag, "starting");
    return true;
}

void FaceDetector::stop()
{
    auto& s = state();
    std::unique_lock<std::mutex> lock(s.mutex);
    if (!s.run.load() && !s.active) {
        return;
    }
    s.run.store(false);
    s.stops++;
    s.cv.notify_all();
    // The current run (tens of ms) or a model load in progress has to finish first
    if (!s.cv.wait_for(lock, std::chrono::milliseconds(kStopTimeoutMs), [&]() { return !s.active; })) {
        mclog::tagError(kTag, "detector did not stop within {} ms", kStopTimeoutMs);
    }
}

bool FaceDetector::isRunning() const
{
    return state().run.load();
}

bool FaceDetector::latest(uint32_t afterSeq, FaceFrame& out)
{
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.latest.seq == 0 || s.latest.seq == afterSeq) {
        return false;
    }
    out = s.latest;
    return true;
}

FaceDetectorStats FaceDetector::stats() const
{
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.stats;
}

}  // namespace stackpet::vision
#endif  // ESP_PLATFORM
