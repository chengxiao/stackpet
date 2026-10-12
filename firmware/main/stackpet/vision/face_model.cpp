/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef ESP_PLATFORM
#include "face_model.h"
#include "face_scoring.h"
#include <dl_detect_msr_postprocessor.hpp>
#include <dl_image_preprocessor.hpp>
#include <dl_model_base.hpp>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <mooncake_log.h>
#if STACKPET_FD_BENCH
#include <fmt/format.h>
#include <string>
#endif
#include <algorithm>
#include <array>
#include <functional>
#include <new>

// The packed MSR + MNP models that the human_face_detect component embeds (CONFIG_HUMAN_FACE_DETECT_MODEL_IN_FLASH_RODATA)
extern const uint8_t human_face_detect_espdl[] asm("_binary_human_face_detect_espdl_start");

namespace stackpet::vision {

namespace {

constexpr const char* kMsrModel = "human_face_detect_msr_s8_v1.espdl";
constexpr const char* kMnpModel = "human_face_detect_mnp_s8_v1.espdl";
constexpr float kNmsThr         = 0.5f;  // human_face_detect defaults for both stages
constexpr int kTopK             = 10;
constexpr int kMnpFaceClass     = 1;  // MNP class logits: [background, face]

float ms_since(int64_t startUs)
{
    return (esp_timer_get_time() - startUs) / 1000.0f;
}

size_t fit_internal_budget(size_t wanted)
{
    InternalHeap heap;
    heap.freeBytes   = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    heap.simdFree    = heap_caps_get_free_size(kFaceModelInternalCaps);
    heap.simdLargest = heap_caps_get_largest_free_block(kFaceModelInternalCaps);
    return fitInternalBudget(wanted, heap);
}

// esp-dl does not report a failed tensor allocation (the model is built without tensors): check before use
bool tensors_allocated(dl::Model& model)
{
    auto& inputs  = model.get_inputs();
    auto& outputs = model.get_outputs();
    if (inputs.size() != 1 || outputs.empty()) {
        return false;
    }
    for (const auto& io : {std::cref(inputs), std::cref(outputs)}) {
        for (const auto& [name, tensor] : io.get()) {
            if (tensor == nullptr || tensor->data == nullptr) {
                return false;
            }
        }
    }
    return true;
}

/**
 * Stage 2 postprocessing with the face class probability as the score.
 *
 * MNP outputs, per anchor, the logits of two classes (softmax), a box and 5 landmarks. esp-dl's MNPPostprocessor (3.3,
 * unchanged on master) keeps a candidate when its most likely class is likely enough, whichever class that is: a
 * background crop passes too, with its background probability as the "face" score, so stage 2 never rejected
 * anything. Same box decoding as MNPPostprocessor, without the landmarks (StackPet does not use them).
 */
class FaceMnpPostprocessor : public dl::detect::AnchorBoxDetectPostprocessor {
public:
    using AnchorBoxDetectPostprocessor::AnchorBoxDetectPostprocessor;

    void postprocess() override
    {
        dl::TensorBase* score = m_model->get_output("score");
        dl::TensorBase* box   = m_model->get_output("box");
        if (score->dtype == dl::DATA_TYPE_INT8) {
            parse<int8_t>(*score, *box);
        } else {
            parse<int16_t>(*score, *box);
        }
    }

private:
    template <typename T>
    void parse(dl::TensorBase& score, dl::TensorBase& box)
    {
        const auto& anchors     = m_stages[0].anchor_shape;
        const int height        = score.shape[1];
        const int width         = score.shape[2];
        const int anchor_count  = static_cast<int>(anchors.size());
        const int classes       = score.shape[3] / anchor_count;
        const float score_scale = DL_SCALE(score.exponent);
        const float box_scale   = DL_SCALE(box.exponent);
        const float inv_scale_x = m_image_preprocessor->get_resize_scale_x(true);
        const float inv_scale_y = m_image_preprocessor->get_resize_scale_y(true);
        const int left          = m_image_preprocessor->get_crop_area_top_left_x();
        const int top           = m_image_preprocessor->get_crop_area_top_left_y();
        const T* score_ptr      = static_cast<const T*>(score.data);
        const T* box_ptr        = static_cast<const T*>(box.data);

        for (int i = 0; i < height * width * anchor_count; i++, score_ptr += classes, box_ptr += 4) {
            const float face = classProbability(score_ptr, classes, score_scale, classes > 1 ? kMnpFaceClass : 0);
#if STACKPET_FD_BENCH
            log_scores(score_ptr, classes, score_scale, face);
#endif
            if (!aboveThreshold(face, m_score_thr)) {
                continue;
            }
            const auto& anchor = anchors[i % anchor_count];
            const float anchor_h = static_cast<float>(anchor[0]);
            const float anchor_w = static_cast<float>(anchor[1]);
            dl::detect::result_t result;
            result.category = 0;
            result.score    = face;
            result.box      = {
                static_cast<int>(anchor_w * (box_ptr[0] * box_scale) * inv_scale_x + left),
                static_cast<int>(anchor_h * (box_ptr[1] * box_scale) * inv_scale_y + top),
                static_cast<int>((anchor_w * (box_ptr[2] * box_scale) + anchor_w) * inv_scale_x + left),
                static_cast<int>((anchor_h * (box_ptr[3] * box_scale) + anchor_h) * inv_scale_y + top)};
            m_box_list.insert(std::upper_bound(m_box_list.begin(), m_box_list.end(), result, dl::detect::greater_box),
                              result);
        }
    }

#if STACKPET_FD_BENCH
    // Test builds: the raw class scores of the first evaluations, to check which class is the face
    template <typename T>
    static void log_scores(const T* scores, int classes, float scale, float face)
    {
        static int logged = 0;
        if (logged >= 12) {
            return;
        }
        logged++;
        std::string values;
        for (int c = 0; c < classes; c++) {
            values += fmt::format(" {:.2f}", scores[c] * scale);
        }
        mclog::tagInfo("FaceModel", "mnp logits:{} -> face {:.3f}", values, face);
    }
#endif
};

}  // namespace

// One esp-dl model with its pre- and postprocessor. Members are destroyed in reverse order: the postprocessor and the
// preprocessor (which point at the model's tensors) go before the model.
struct FaceModel::Stage {
    static std::unique_ptr<dl::Model> load(const char* name, size_t internalBytes)
    {
        return std::make_unique<dl::Model>(reinterpret_cast<const char*>(human_face_detect_espdl), name,
                                           fbs::MODEL_LOCATION_IN_FLASH_RODATA, static_cast<int>(internalBytes));
    }

    std::unique_ptr<dl::Model> model;
    std::unique_ptr<dl::image::ImagePreprocessor> preprocessor;
    std::unique_ptr<dl::detect::DetectPostprocessor> postprocessor;
    size_t internalBytes = 0;

    Stage(const char* name, size_t wantedInternalBytes) : internalBytes(fit_internal_budget(wantedInternalBytes))
    {
        model = load(name, internalBytes);
        if (!tensors_allocated(*model) && internalBytes > 0) {
            // The budget block was gone by the time esp-dl allocated it (the model's own small objects come first,
            // from the same regions): everything in PSRAM then
            mclog::tagWarn("FaceModel", "{}: no {} K internal block after all, feature maps in PSRAM", name,
                           internalBytes / 1024);
            model.reset();
            internalBytes = 0;
            model         = load(name, 0);
        }
        if (!tensors_allocated(*model)) {
            throw std::bad_alloc();
        }
        model->minimize();
        // Same normalization as human_face_detect: raw 0-255 values, R/B swapped
        preprocessor = std::make_unique<dl::image::ImagePreprocessor>(model.get(), std::array<float, 3>{0, 0, 0},
                                                                      std::array<float, 3>{1, 1, 1}, true);
    }
};

FaceModel::FaceModel(const FaceModelConfig& config) : _maxCandidates(std::max(config.maxCandidates, 1))
{
    _msr = std::make_unique<Stage>(kMsrModel, config.msrInternalBytes);
    _msr->postprocessor.reset(new dl::detect::MSRPostprocessor(
        _msr->model.get(), _msr->preprocessor.get(), config.msrScoreThr, kNmsThr, kTopK,
        {{8, 8, 9, 9, {{16, 16}, {32, 32}}}, {16, 16, 9, 9, {{64, 64}, {128, 128}}}}));

    _mnp = std::make_unique<Stage>(kMnpModel, config.mnpInternalBytes);
    _mnp->postprocessor.reset(new FaceMnpPostprocessor(_mnp->model.get(), _mnp->preprocessor.get(),
                                                       config.mnpScoreThr, kNmsThr, kTopK, {{1, 1, 0, 0, {{48, 48}}}}));
}

FaceModel::~FaceModel() = default;

size_t FaceModel::msrInternalBytes() const
{
    return _msr->internalBytes;
}

size_t FaceModel::mnpInternalBytes() const
{
    return _mnp->internalBytes;
}

std::list<dl::detect::result_t>& FaceModel::run(const dl::image::img_t& img, FaceModelTiming* timing)
{
    // Stage 1 (MSR): proposals on the whole image, best first (human_face_detect::MSR / DetectImpl::run)
    int64_t start = esp_timer_get_time();
    auto& msr     = *_msr;
    msr.preprocessor->preprocess(img);
    msr.model->run();
    msr.postprocessor->clear_result();
    msr.postprocessor->postprocess();
    std::list<dl::detect::result_t>& candidates = msr.postprocessor->get_result(img.width, img.height);
    const float msr_ms                          = ms_since(start);

    // Stage 2 (MNP): refine a square crop around each of the best candidates (human_face_detect::MNP::run)
    start     = esp_timer_get_time();
    auto& mnp = *_mnp;
    mnp.postprocessor->clear_result();
    int runs = 0;
    for (auto& candidate : candidates) {
        if (runs >= _maxCandidates) {
            break;
        }
        runs++;
        const int center_x = (candidate.box[0] + candidate.box[2]) >> 1;
        const int center_y = (candidate.box[1] + candidate.box[3]) >> 1;
        const int side     = std::max(candidate.box[2] - candidate.box[0], candidate.box[3] - candidate.box[1]);
        candidate.box[0]   = center_x - (side >> 1);
        candidate.box[1]   = center_y - (side >> 1);
        candidate.box[2]   = candidate.box[0] + side;
        candidate.box[3]   = candidate.box[1] + side;
        candidate.limit_box(img.width, img.height);

        mnp.preprocessor->preprocess(img, candidate.box);
        mnp.model->run();
        mnp.postprocessor->postprocess();
    }
    mnp.postprocessor->nms();
    std::list<dl::detect::result_t>& faces = mnp.postprocessor->get_result(img.width, img.height);

    if (timing != nullptr) {
        timing->msrMs      = msr_ms;
        timing->mnpMs      = ms_since(start);
        timing->candidates = runs;
    }
    return faces;
}

}  // namespace stackpet::vision
#endif  // ESP_PLATFORM
