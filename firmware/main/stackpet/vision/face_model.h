/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#ifdef ESP_PLATFORM
#include <dl_detect_define.hpp>
#include <dl_image_define.hpp>
#include <esp_heap_caps.h>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>

namespace stackpet::vision {

// What esp-dl allocates feature maps with (dl::tool::calloc_aligned adds MALLOC_CAP_SIMD): on the S3 only the D/IRAM
// qualifies, not the internal reserve pool, RTC RAM or IRAM
inline constexpr uint32_t kFaceModelInternalCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_SIMD;

struct FaceModelConfig {
    // Internal RAM budget for each stage's feature maps. esp-dl places tensors there first and the rest in PSRAM;
    // 0 = everything in PSRAM (what human_face_detect does). The budget is one contiguous block, allocated when the
    // stage loads; it is reduced (down to 0) so the heap keeps its reserves (face_scoring.h), and the stage falls
    // back to PSRAM only when the block is gone by the time esp-dl allocates it. See msrInternalBytes().
    size_t msrInternalBytes = 0;
    size_t mnpInternalBytes = 0;
    float msrScoreThr       = 0.5f;  // human_face_detect defaults
    float mnpScoreThr       = 0.5f;
    // MSR proposals (best first) refined by MNP: every one costs a full MNP run
    int maxCandidates = 10;
};

struct FaceModelTiming {
    float msrMs    = 0.0f;  // stage 1: preprocess + model + postprocess
    float mnpMs    = 0.0f;  // stage 2, all candidates
    int candidates = 0;     // MNP runs
};

/**
 * @brief The human_face_detect MSR+MNP two-stage detector (int8, models from the component's flash rodata), built
 * from the same esp-dl parts as human_face_detect::MSRMNP v0.5.0 (same anchors, preprocessing and thresholds), plus
 * an internal RAM budget for the feature maps, a cap on the stage 2 candidates and per-stage timing.
 *
 * The constructor loads both models (throws std::bad_alloc when out of memory). Not thread-safe: one task runs it.
 */
class FaceModel {
public:
    explicit FaceModel(const FaceModelConfig& config = FaceModelConfig());
    ~FaceModel();
    FaceModel(const FaceModel&)            = delete;
    FaceModel& operator=(const FaceModel&) = delete;

    // Faces in img, best score first, boxes in img pixels. The list belongs to the model (valid until the next run).
    std::list<dl::detect::result_t>& run(const dl::image::img_t& img, FaceModelTiming* timing = nullptr);

    // The internal RAM budgets actually used: the configured ones, reduced to what the heap could spare at load time
    size_t msrInternalBytes() const;
    size_t mnpInternalBytes() const;

private:
    struct Stage;
    std::unique_ptr<Stage> _msr;
    std::unique_ptr<Stage> _mnp;
    int _maxCandidates;
};

}  // namespace stackpet::vision
#endif  // ESP_PLATFORM
