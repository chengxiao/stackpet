/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#if defined(ESP_PLATFORM) && defined(STACKPET_FD_BENCH) && STACKPET_FD_BENCH
#include "face_bench.h"
#include "face_detector.h"
#include "face_model.h"
#include <stackpet/camera/frame_grabber.h>
#include <esp_heap_caps.h>
#include <esp_jpeg_dec.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mooncake_log.h>
#include <sdkconfig.h>
#include <fmt/format.h>
#include <algorithm>
#include <memory>
#include <new>
#include <string>
#include <vector>

// The positive probe (testdata/face_probe.jpg, embedded in bench builds only)
extern const uint8_t face_probe_jpg_start[] asm("_binary_face_probe_jpg_start");
extern const uint8_t face_probe_jpg_end[] asm("_binary_face_probe_jpg_end");

namespace stackpet::vision {

namespace {

const char* kTag = "FaceBench";

constexpr uint32_t kStackSize    = 16 * 1024;  // = the FaceDetector task
constexpr UBaseType_t kPriority  = 1;          // = the FaceDetector task (and the mooncake main loop)
constexpr BaseType_t kCore       = 0;
constexpr int kWarmupRuns        = 2;
constexpr int kRuns              = 15;
constexpr uint32_t kFrameWaitMs  = 500;
constexpr int kMaxFrameMisses    = 10;
constexpr uint32_t kStartDelayMs = 4000;  // let the app and the camera settle first
constexpr size_t K               = 1024;
constexpr int kSyntheticWidth    = 320;  // the gray / black images, = a camera frame
constexpr int kSyntheticHeight   = 240;

// Where the face is in the probe (testdata/README.md): every run's best face must overlap it this much
constexpr FaceBox kProbeFace{105, 70, 225, 185, 0.0f};
constexpr float kProbeMinIou = 0.4f;

// What the model gets. Everything but Camera is prepared outside the timed part.
enum class Input {
    Camera,  // the 320x240 RGB565 frame as is
    Mirror,  // flipped left/right: detections that stay at the same edge do not come from the scene
    Gray,    // uniform mid gray / black
    Black,
    Face,    // the embedded face photo
};

// What a correct detector must find (camera frames show whatever is in the room: no verdict)
enum class Expect { Anything, NoFace, ProbeFace };

struct Variant {
    const char* name;
    bool internalStack;
    FaceModelConfig model;
    Input input   = Input::Camera;
    Expect expect = Expect::Anything;
};

FaceModelConfig model_config(size_t msrK, size_t mnpK, int cap = 10, float msrThr = 0.5f)
{
    FaceModelConfig config;
    config.msrInternalBytes = msrK * K;
    config.mnpInternalBytes = mnpK * K;
    config.maxCandidates    = cap;
    config.msrScoreThr      = msrThr;
    return config;
}

// "base" = human_face_detect's own setup (everything in PSRAM, 10 candidates); "detector" = what the FaceDetector
// uses (32 KB MSR budget, 3 candidates). The probes come first, so the logged MNP logits are the face probe's.
// "mnp cost" takes every MSR proposal (threshold 0.01) to time stage 2 without a face in view. The measurements of
// the stack location, the 160x120 input and an MNP budget are in the README (they made no or little difference).
const Variant kVariants[] = {
    {"probe face", false, model_config(0, 0), Input::Face, Expect::ProbeFace},
    {"probe face/d", false, model_config(32, 0, 3), Input::Face, Expect::ProbeFace},
    {"probe gray", false, model_config(32, 0, 3), Input::Gray, Expect::NoFace},
    {"probe black", false, model_config(32, 0, 3), Input::Black, Expect::NoFace},
    {"base", false, model_config(0, 0)},
    {"detector", false, model_config(32, 0, 3)},
    {"probe mirror", false, model_config(32, 0, 3), Input::Mirror},
    {"mnp cost", false, model_config(0, 0, 10, 0.01f)},
    {"base again", false, model_config(0, 0)},
};

struct Probe {
    const uint16_t* pixels = nullptr;  // RGB565 LE, decoded once, never freed (the bench runs once per boot)
    int width              = 0;
    int height             = 0;
};
Probe g_probe;

struct Job {
    const Variant* variant = nullptr;
    SemaphoreHandle_t done = nullptr;
    std::string summary;
    bool judged = false;  // the variant has an expectation
    bool passed = false;
};

int kb_delta(size_t before, size_t after)
{
    return static_cast<int>((static_cast<int64_t>(before) - static_cast<int64_t>(after)) / static_cast<int64_t>(K));
}

bool decode_probe()
{
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type       = JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_handle_t handle = nullptr;
    if (jpeg_dec_open(&config, &handle) != JPEG_ERR_OK) {
        return false;
    }
    jpeg_dec_io_t io             = {};
    jpeg_dec_header_info_t info  = {};
    io.inbuf                     = const_cast<uint8_t*>(face_probe_jpg_start);
    io.inbuf_len                 = static_cast<int>(face_probe_jpg_end - face_probe_jpg_start);
    int out_len                  = 0;
    uint8_t* out                 = nullptr;
    if (jpeg_dec_parse_header(handle, &io, &info) == JPEG_ERR_OK &&
        jpeg_dec_get_outbuf_len(handle, &out_len) == JPEG_ERR_OK && out_len > 0) {
        // The decoder wants a 16-byte aligned output buffer
        out       = static_cast<uint8_t*>(heap_caps_aligned_calloc(16, 1, out_len, MALLOC_CAP_SPIRAM));
        io.outbuf = out;
        if (out != nullptr && jpeg_dec_process(handle, &io) != JPEG_ERR_OK) {
            heap_caps_free(out);
            out = nullptr;
        }
    }
    jpeg_dec_close(handle);
    if (out == nullptr) {
        return false;
    }
    g_probe.pixels = reinterpret_cast<const uint16_t*>(out);
    g_probe.width  = info.width;
    g_probe.height = info.height;
    return true;
}

float iou(const dl::detect::result_t& face, const FaceBox& box)
{
    const int x0    = std::max(face.box[0], box.x0);
    const int y0    = std::max(face.box[1], box.y0);
    const int x1    = std::min(face.box[2], box.x1);
    const int y1    = std::min(face.box[3], box.y1);
    const float in  = static_cast<float>(std::max(0, x1 - x0) * std::max(0, y1 - y0));
    const float a   = static_cast<float>((face.box[2] - face.box[0]) * (face.box[3] - face.box[1]));
    const float b   = static_cast<float>((box.x1 - box.x0) * (box.y1 - box.y0));
    const float all = a + b - in;
    return all > 0.0f ? in / all : 0.0f;
}

void mirror(const uint16_t* src, int width, int height, uint16_t* dst)
{
    for (int y = 0; y < height; y++) {
        const uint16_t* row = src + static_cast<size_t>(y) * width;
        uint16_t* out       = dst + static_cast<size_t>(y) * width;
        for (int x = 0; x < width; x++) {
            out[x] = row[width - 1 - x];
        }
    }
}

void run_variant(Job& job)
{
    const Variant& v          = *job.variant;
    const size_t int_before   = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t block_before = heap_caps_get_largest_free_block(kFaceModelInternalCaps);
    const size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    job.judged                = v.expect != Expect::Anything;

    if (v.input == Input::Face && g_probe.pixels == nullptr) {
        job.summary = fmt::format("{:<13} the probe image could not be decoded", v.name);
        return;
    }

    std::unique_ptr<FaceModel> model;
    const int64_t load_start = esp_timer_get_time();
    try {
        model = std::make_unique<FaceModel>(v.model);
    } catch (const std::exception&) {
        job.summary = fmt::format("{:<13} model allocation failed (internal free {} K, largest SIMD block {} K)",
                                  v.name, int_before / K, block_before / K);
        return;
    }
    const float load_ms = (esp_timer_get_time() - load_start) / 1000.0f;
    const int model_int = kb_delta(int_before, heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    const int model_ps  = kb_delta(psram_before, heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    auto& grabber = FrameGrabber::instance();
    std::unique_ptr<uint16_t, decltype(&heap_caps_free)> work(nullptr, heap_caps_free);  // mirror / gray / black
    const bool from_camera = v.input == Input::Camera || v.input == Input::Mirror;
    uint32_t seq           = 0;
    int done = 0, runs = 0, misses = 0, cand_sum = 0, face_sum = 0, best_runs = 0, probe_hits = 0;
    float total_sum = 0, total_max = 0, msr_sum = 0, mnp_sum = 0, best_sum = 0, worst_iou = 1.0f;
    int width = 0, height = 0;
    std::string last_boxes;  // faces of the last run, image pixels

    while (done < kWarmupRuns + kRuns) {
        dl::image::img_t img;
        img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE;
        RgbFrame frame;  // keeps a camera frame alive while it is analyzed
        if (from_camera) {
            if (!grabber.waitRgb(seq, frame, kFrameWaitMs)) {
                if (++misses > kMaxFrameMisses) {
                    job.summary = fmt::format("{:<13} no camera frames", v.name);
                    return;
                }
                continue;
            }
            seq = frame.seq;
            grabber.requestRgb();  // the next frame converts on core 1 meanwhile, as in the FaceDetector
            if (!frame.pixels || frame.width < 2 || frame.height < 2) {
                continue;
            }
            img.data   = const_cast<uint16_t*>(frame.pixels.get());
            img.width  = frame.width;
            img.height = frame.height;
        } else if (v.input == Input::Face) {
            img.data   = const_cast<uint16_t*>(g_probe.pixels);
            img.width  = g_probe.width;
            img.height = g_probe.height;
        } else {
            img.width  = kSyntheticWidth;
            img.height = kSyntheticHeight;
        }

        if (v.input == Input::Mirror || v.input == Input::Gray || v.input == Input::Black) {
            const size_t count = static_cast<size_t>(img.width) * img.height;
            if (!work) {
                work.reset(static_cast<uint16_t*>(heap_caps_malloc(count * sizeof(uint16_t), MALLOC_CAP_SPIRAM)));
                if (!work) {
                    job.summary = fmt::format("{:<13} no PSRAM for the input image", v.name);
                    return;
                }
            }
            if (v.input == Input::Mirror) {
                mirror(frame.pixels.get(), frame.width, frame.height, work.get());
            } else {
                std::fill_n(work.get(), count, static_cast<uint16_t>(v.input == Input::Gray ? 0x8410 : 0));
            }
            img.data = work.get();
        }
        width  = img.width;
        height = img.height;

        FaceModelTiming timing;
        const int64_t start = esp_timer_get_time();
        auto& faces         = model->run(img, &timing);
        const float total   = (esp_timer_get_time() - start) / 1000.0f;
        last_boxes.clear();
        for (const auto& face : faces) {
            last_boxes += fmt::format(" [{},{},{},{} {:.2f}]", face.box[0], face.box[1], face.box[2], face.box[3],
                                      face.score);
        }
        if (++done > kWarmupRuns) {
            runs++;
            total_sum += total;
            total_max = std::max(total_max, total);
            msr_sum += timing.msrMs;
            mnp_sum += timing.mnpMs;
            cand_sum += timing.candidates;
            face_sum += static_cast<int>(faces.size());
            if (!faces.empty()) {
                best_sum += faces.front().score;
                best_runs++;
            }
            if (v.expect == Expect::ProbeFace) {
                const float overlap = faces.empty() ? 0.0f : iou(faces.front(), kProbeFace);
                worst_iou           = std::min(worst_iou, overlap);
                probe_hits += overlap >= kProbeMinIou ? 1 : 0;
            }
        }
        if (from_camera) {
            vTaskDelay(1);  // as in the FaceDetector: IDLE0 must run (task watchdog)
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));  // no frame wait paces the synthetic inputs
        }
    }

    std::string verdict;
    switch (v.expect) {
        case Expect::Anything:
            break;
        case Expect::NoFace:
            job.passed = face_sum == 0;
            verdict    = job.passed ? " | PASS (no face)" : " | FAIL (faces on a uniform image)";
            break;
        case Expect::ProbeFace:
            job.passed = probe_hits == runs;
            verdict    = fmt::format(" | {} ({}/{} runs on the face, worst IoU {:.2f})", job.passed ? "PASS" : "FAIL",
                                     probe_hits, runs, worst_iou);
            break;
    }

    const float n = static_cast<float>(runs);
    job.summary   = fmt::format(
        "{:<13} stack {:<8} int {:>3}/{:<3}K cap {:>2} thr {:.2f} {}x{}: total {:6.1f} ms (max {:6.1f}) = msr {:6.1f} "
          "+ mnp {:6.1f} ({:.1f} cand, {} ms each) | faces {:.1f} best {:.2f} | load {:.0f} ms, model int {} K psram {} "
          "K, heap int {} K (SIMD block {} K) | last:{}{}",
        v.name, v.internalStack ? "internal" : "psram", model->msrInternalBytes() / K, model->mnpInternalBytes() / K,
        v.model.maxCandidates, v.model.msrScoreThr, width, height, total_sum / n, total_max, msr_sum / n, mnp_sum / n,
        cand_sum / n, cand_sum > 0 ? fmt::format("{:.1f}", mnp_sum / cand_sum) : std::string("-"), face_sum / n,
        best_runs > 0 ? best_sum / best_runs : 0.0f, load_ms, model_int, model_ps, int_before / K, block_before / K,
        last_boxes.empty() ? std::string(" none") : last_boxes, verdict);
}

void variant_task(void* arg)
{
    auto* job = static_cast<Job*>(arg);
    run_variant(*job);
    xSemaphoreGive(job->done);
    vTaskSuspend(nullptr);  // deleted by the bench task
}

void bench_task(void* /*arg*/)
{
    vTaskDelay(pdMS_TO_TICKS(kStartDelayMs));
    mclog::tagInfo(kTag, "start: {} variants x {} runs (+{} warm-up); CPU {} MHz, PSRAM {} MHz {}, D-cache {} K / {} B lines",
                   std::size(kVariants), kRuns, kWarmupRuns, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, CONFIG_SPIRAM_SPEED,
#if CONFIG_SPIRAM_MODE_OCT
                   "octal",
#else
                   "quad",
#endif
                   CONFIG_ESP32S3_DATA_CACHE_SIZE / 1024, CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE);
    mclog::tagInfo(kTag, "heap: internal free {} K (largest {} K), SIMD-capable internal free {} K (largest {} K)",
                   heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / K,
                   heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / K,
                   heap_caps_get_free_size(kFaceModelInternalCaps) / K, heap_caps_get_largest_free_block(kFaceModelInternalCaps) / K);
    if (decode_probe()) {
        mclog::tagInfo(kTag, "face probe: {}x{}, face expected at [{},{},{},{}]", g_probe.width, g_probe.height,
                       kProbeFace.x0, kProbeFace.y0, kProbeFace.x1, kProbeFace.y1);
    } else {
        mclog::tagError(kTag, "face probe: the embedded JPEG could not be decoded");
    }

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    std::vector<std::string> summaries;
    int judged = 0, passed = 0;
    for (const auto& variant : kVariants) {
        const uint32_t caps = variant.internalStack ? (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
                                                    : (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        auto* stack = static_cast<StackType_t*>(heap_caps_malloc(kStackSize, caps));
        auto* tcb   = static_cast<StaticTask_t*>(heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL));
        Job job;
        job.variant         = &variant;
        job.done            = done;
        job.judged          = variant.expect != Expect::Anything;  // a variant that cannot run fails its probe
        TaskHandle_t handle = nullptr;
        if (done != nullptr && stack != nullptr && tcb != nullptr) {
            handle = xTaskCreateStaticPinnedToCore(variant_task, "fd_bench", kStackSize, &job, kPriority, stack, tcb,
                                                   kCore);
        }
        if (handle == nullptr) {
            job.summary = fmt::format("{:<13} could not create the task", variant.name);
        } else {
            xSemaphoreTake(done, portMAX_DELAY);
            vTaskDelete(handle);  // suspended by now (or about to be, it is on this core at this priority)
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        summaries.push_back(job.summary);
        judged += job.judged ? 1 : 0;
        passed += job.judged && job.passed ? 1 : 0;
        mclog::tagInfo(kTag, "{}", summaries.back());
        heap_caps_free(stack);
        heap_caps_free(tcb);
    }

    mclog::tagInfo(kTag, "summary:");
    for (const auto& line : summaries) {
        mclog::tagInfo(kTag, "  {}", line);
    }
    mclog::tagInfo(kTag, "probes {}/{} passed: {}", passed, judged, passed == judged ? "PASS" : "FAIL");
    mclog::tagInfo(kTag, "done");
    if (done != nullptr) {
        vSemaphoreDelete(done);
    }
    vTaskDelete(nullptr);
}

}  // namespace

void startFaceBench()
{
    if (xTaskCreatePinnedToCore(bench_task, "fd_bench_ctl", 6 * 1024, nullptr, kPriority, nullptr, kCore) != pdPASS) {
        mclog::tagError(kTag, "could not start the bench task");
    }
}

}  // namespace stackpet::vision
#endif  // ESP_PLATFORM && STACKPET_FD_BENCH
