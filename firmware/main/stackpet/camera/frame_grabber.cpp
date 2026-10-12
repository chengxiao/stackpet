/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "frame_grabber.h"
#include <hal/board/hal_bridge.h>
#include <esp_jpeg_common.h>
#include <esp_jpeg_enc.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mooncake_log.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>

namespace stackpet {

namespace {

const char* kTag = "FrameGrabber";

// The capture task runs on core 1 below the LVGL task (priority 3, core 1) so the avatar animation stays smooth,
// and away from core 0 where Wi-Fi, lwIP and the mooncake main loop live.
constexpr uint32_t kTaskStackSize   = 6144;
constexpr UBaseType_t kTaskPriority = 2;
constexpr BaseType_t kTaskCore      = 1;
constexpr uint32_t kStopTimeoutMs   = 1500;
constexpr int64_t kStatsWindowUs    = 1000000;
constexpr int64_t kStatsLogPeriodUs = 10000000;
constexpr float kEmaAlpha           = 0.2f;

struct Shared {
    std::mutex mutex;
    std::condition_variable cv;
    GrabberConfig config;
    JpegFrame jpeg;
    LumaFrame luma;
    GrabberStats stats;
    uint32_t jpeg_seq = 0;  // monotonic across restarts, so stale afterSeq values from a previous run still work
    uint32_t luma_seq = 0;
    std::condition_variable rgb_cv;
    RgbFrame rgb;
    uint32_t rgb_seq = 0;
};

Shared& shared()
{
    static Shared instance;
    return instance;
}

std::mutex _lifecycle_mutex;
std::atomic<bool> _running{false};
std::atomic<bool> _stop_requested{false};
std::atomic<int> _jpeg_consumers{0};
std::atomic<uint8_t> _jpeg_quality{50};
std::atomic<bool> _rgb_requested{false};

float ema(float current, float sample)
{
    return current <= 0.0f ? sample : current + kEmaAlpha * (sample - current);
}

int64_t now_us()
{
    return esp_timer_get_time();
}

size_t bytes_per_pixel(uint32_t format)
{
    switch (format) {
        case V4L2_PIX_FMT_YUYV:
        case V4L2_PIX_FMT_RGB565:
            return 2;
        case V4L2_PIX_FMT_RGB24:
            return 3;
        case V4L2_PIX_FMT_GREY:
            return 1;
        default:
            return 0;
    }
}

bool to_jpeg_format(uint32_t format, jpeg_pixel_format_t& out)
{
    switch (format) {
        case V4L2_PIX_FMT_YUYV:
            out = JPEG_PIXEL_FORMAT_YCbYCr;
            return true;
        case V4L2_PIX_FMT_RGB565:
            out = JPEG_PIXEL_FORMAT_RGB565_LE;
            return true;
        case V4L2_PIX_FMT_RGB24:
            out = JPEG_PIXEL_FORMAT_RGB888;
            return true;
        case V4L2_PIX_FMT_GREY:
            out = JPEG_PIXEL_FORMAT_GRAY;
            return true;
        default:
            return false;
    }
}

/**
 * Box-filter the luma of a frame down to a small grid (8x8 blocks for 320x240 -> 40x30).
 * Averaging every pixel of a block keeps the sensor noise low for the frame-difference motion detector.
 */
bool downsample_luma(const uint8_t* src, int width, int height, uint32_t format, uint8_t* dst, int dstWidth,
                     int dstHeight, std::vector<uint32_t>& accum)
{
    const int block_w = width / dstWidth;
    const int block_h = height / dstHeight;
    if (block_w <= 0 || block_h <= 0) {
        return false;
    }
    const uint32_t block_area = static_cast<uint32_t>(block_w * block_h);
    const size_t bpp          = bytes_per_pixel(format);
    const size_t stride       = static_cast<size_t>(width) * bpp;
    accum.assign(dstWidth, 0);

    for (int by = 0; by < dstHeight; by++) {
        std::fill(accum.begin(), accum.end(), 0);
        for (int y = by * block_h; y < (by + 1) * block_h; y++) {
            const uint8_t* row = src + static_cast<size_t>(y) * stride;
            for (int bx = 0; bx < dstWidth; bx++) {
                uint32_t sum = 0;
                const int x0 = bx * block_w;
                const int x1 = x0 + block_w;
                switch (format) {
                    case V4L2_PIX_FMT_YUYV:
                        // Y0 U Y1 V: luma is every even byte
                        for (int x = x0; x < x1; x++) {
                            sum += row[2 * x];
                        }
                        break;
                    case V4L2_PIX_FMT_RGB565:
                        for (int x = x0; x < x1; x++) {
                            uint16_t px = static_cast<uint16_t>(row[2 * x] | (row[2 * x + 1] << 8));
                            uint32_t r  = (px >> 11) << 3;
                            uint32_t g  = ((px >> 5) & 0x3F) << 2;
                            uint32_t b  = (px & 0x1F) << 3;
                            sum += (77 * r + 150 * g + 29 * b) >> 8;
                        }
                        break;
                    case V4L2_PIX_FMT_RGB24:
                        for (int x = x0; x < x1; x++) {
                            const uint8_t* p = row + 3 * x;
                            sum += (77u * p[0] + 150u * p[1] + 29u * p[2]) >> 8;
                        }
                        break;
                    case V4L2_PIX_FMT_GREY:
                        for (int x = x0; x < x1; x++) {
                            sum += row[x];
                        }
                        break;
                    default:
                        return false;
                }
                accum[bx] += sum;
            }
        }
        for (int bx = 0; bx < dstWidth; bx++) {
            dst[by * dstWidth + bx] = static_cast<uint8_t>(accum[bx] / block_area);
        }
    }
    return true;
}

/**
 * Convert a frame to RGB565 (sensor resolution / downscale) into a fresh PSRAM buffer and publish it. Consumers
 * keep their frame alive through the shared pointer, so a buffer is never reused while somebody still reads it.
 */
bool publish_rgb(const uint8_t* frame, uint16_t width, uint16_t height, uint32_t format, uint8_t downscale,
                 int64_t frame_at)
{
    static bool format_warned = false;
    if (format != V4L2_PIX_FMT_YUYV && format != V4L2_PIX_FMT_RGB565) {
        if (!format_warned) {
            mclog::tagError(kTag, "rgb output does not support format 0x{:08x}", format);
            format_warned = true;
        }
        return false;
    }

    const int scale       = downscale == 2 ? 2 : 1;
    const uint16_t out_w  = width / scale;
    const uint16_t out_h  = height / scale;
    const size_t count    = static_cast<size_t>(out_w) * out_h;
    auto* buffer = static_cast<uint16_t*>(heap_caps_malloc(count * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == nullptr) {
        static uint32_t failures = 0;
        if (failures++ % 50 == 0) {
            mclog::tagWarn(kTag, "no memory for a {}x{} rgb frame ({} times)", out_w, out_h, failures);
        }
        return false;
    }

    if (format == V4L2_PIX_FMT_YUYV) {
        pixel::yuyvToRgb565(frame, width, height, scale, buffer);
    } else if (scale == 1) {
        memcpy(buffer, frame, count * sizeof(uint16_t));
    } else {
        const auto* src = reinterpret_cast<const uint16_t*>(frame);
        for (uint16_t y = 0; y < out_h; y++) {
            for (uint16_t x = 0; x < out_w; x++) {
                buffer[static_cast<size_t>(y) * out_w + x] = src[static_cast<size_t>(y) * scale * width + x * scale];
            }
        }
    }

    std::shared_ptr<const uint16_t> pixels(buffer, [](const uint16_t* p) { heap_caps_free(const_cast<uint16_t*>(p)); });
    auto& state = shared();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.rgb.pixels      = std::move(pixels);
        state.rgb.width       = out_w;
        state.rgb.height      = out_h;
        state.rgb.seq         = ++state.rgb_seq;
        state.rgb.timestampUs = frame_at;
    }
    state.rgb_cv.notify_all();
    return true;
}

/**
 * esp_new_jpeg encoder kept open between frames (the xiaozhi image_to_jpeg() helper re-opens the encoder and
 * allocates ~280 KB of scratch buffers for every single image).
 */
class JpegEncoder {
public:
    ~JpegEncoder()
    {
        close();
        if (_out) {
            heap_caps_free(_out);
        }
        if (_aligned_in) {
            jpeg_free_align(_aligned_in);
        }
    }

    bool prepare(int width, int height, jpeg_pixel_format_t format, uint8_t quality)
    {
        if (_handle && width == _width && height == _height && format == _format) {
            if (quality != _quality) {
                if (jpeg_enc_set_quality(_handle, quality) == JPEG_ERR_OK) {
                    _quality = quality;
                } else {
                    close();
                    return prepare(width, height, format, quality);
                }
            }
            return true;
        }

        close();
        jpeg_enc_config_t config = DEFAULT_JPEG_ENC_CONFIG();
        config.width             = width;
        config.height            = height;
        config.src_type          = format;
        config.subsampling       = format == JPEG_PIXEL_FORMAT_GRAY ? JPEG_SUBSAMPLE_GRAY : JPEG_SUBSAMPLE_420;
        config.quality           = quality;
        config.rotate            = JPEG_ROTATE_0D;
        config.task_enable       = false;
        jpeg_error_t ret         = jpeg_enc_open(&config, &_handle);
        if (ret != JPEG_ERR_OK || _handle == nullptr) {
            mclog::tagError(kTag, "jpeg_enc_open failed: {}", static_cast<int>(ret));
            _handle = nullptr;
            return false;
        }
        _width   = width;
        _height  = height;
        _format  = format;
        _quality = quality;
        return ensure_output(static_cast<size_t>(width) * height);
    }

    // Returns the JPEG size, or -1 on failure. The result stays valid until the next call.
    int encode(const uint8_t* src, size_t srcLen)
    {
        if (!_handle) {
            return -1;
        }
        // esp_new_jpeg requires a 16-byte aligned input on the ESP32-S3 (the V4L2 buffers are cache-line aligned)
        if (reinterpret_cast<uintptr_t>(src) & 0xF) {
            if (!_aligned_in || _aligned_in_size < srcLen) {
                if (_aligned_in) {
                    jpeg_free_align(_aligned_in);
                }
                _aligned_in      = static_cast<uint8_t*>(jpeg_calloc_align(srcLen, 16));
                _aligned_in_size = _aligned_in ? srcLen : 0;
                if (!_aligned_in) {
                    return -1;
                }
            }
            memcpy(_aligned_in, src, srcLen);
            src = _aligned_in;
        }

        for (int attempt = 0; attempt < 2; attempt++) {
            int out_len      = 0;
            jpeg_error_t ret = jpeg_enc_process(_handle, src, static_cast<int>(srcLen), _out,
                                                static_cast<int>(_out_size), &out_len);
            if (ret == JPEG_ERR_OK && out_len > 0) {
                return out_len;
            }
            // Most likely the output buffer was too small for a very detailed frame: grow once and retry
            if (attempt == 0 && !ensure_output(_out_size * 2)) {
                break;
            }
            mclog::tagDebug(kTag, "jpeg_enc_process failed: {}, retrying", static_cast<int>(ret));
        }
        return -1;
    }

    const uint8_t* output() const
    {
        return _out;
    }

    void close()
    {
        if (_handle) {
            jpeg_enc_close(_handle);
            _handle = nullptr;
        }
    }

private:
    bool ensure_output(size_t size)
    {
        size = std::max<size_t>(size, 32 * 1024);
        if (_out && _out_size >= size) {
            return true;
        }
        if (_out) {
            heap_caps_free(_out);
        }
        _out      = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        _out_size = _out ? size : 0;
        if (!_out) {
            mclog::tagError(kTag, "failed to allocate {} bytes for jpeg output", size);
        }
        return _out != nullptr;
    }

    jpeg_enc_handle_t _handle   = nullptr;
    int _width                  = 0;
    int _height                 = 0;
    jpeg_pixel_format_t _format = JPEG_PIXEL_FORMAT_YCbYCr;
    uint8_t _quality            = 0;
    uint8_t* _out               = nullptr;
    size_t _out_size            = 0;
    uint8_t* _aligned_in        = nullptr;
    size_t _aligned_in_size     = 0;
};

/**
 * The capture loop. Every frame is dequeued as soon as the sensor delivers it (20 fps) so the V4L2 queue never
 * holds a stale frame; luma and JPEG are computed directly from the driver buffer (no 150 KB copy) and the JPEG
 * rate is limited separately. All locals live in this function so they are destroyed before the task deletes
 * itself.
 */
void run_capture_loop(StackChanCamera* camera)
{
    auto& state = shared();

    JpegEncoder encoder;
    std::vector<uint32_t> accum;
    std::vector<uint8_t> luma;

    int64_t next_jpeg_due_us = 0;
    int64_t window_start_us  = now_us();
    int64_t last_log_us      = window_start_us;
    uint32_t window_captures = 0;
    uint32_t window_jpegs    = 0;
    uint32_t window_rgbs     = 0;
    uint32_t failure_count   = 0;
    bool format_warned       = false;
    float capture_ms         = 0.0f;
    float encode_ms          = 0.0f;
    float capture_fps        = 0.0f;
    float jpeg_fps           = 0.0f;
    float rgb_fps            = 0.0f;
    float rgb_ms             = 0.0f;
    uint32_t last_jpeg_bytes = 0;

    while (!_stop_requested.load()) {
        GrabberConfig config;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            config = state.config;
        }

        int64_t wait_start = now_us();
        int64_t frame_at   = 0;
        bool visited       = camera->VisitFrame([&](const uint8_t* frame, size_t size, uint16_t width,
                                                    uint16_t height, v4l2_pix_fmt_t fmt) {
            frame_at       = now_us();
            const size_t bpp = bytes_per_pixel(fmt);
            if (frame == nullptr || bpp == 0 || size < static_cast<size_t>(width) * height * bpp) {
                if (!format_warned) {
                    mclog::tagError(kTag, "unexpected frame: format 0x{:08x}, {}x{}, {} bytes", fmt, width, height,
                                    size);
                    format_warned = true;
                }
                return;
            }

            // Downscaled luma for the vision consumers (every frame, only if requested)
            if (config.lumaWidth > 0 && config.lumaHeight > 0) {
                luma.resize(static_cast<size_t>(config.lumaWidth) * config.lumaHeight);
                if (downsample_luma(frame, width, height, fmt, luma.data(), config.lumaWidth, config.lumaHeight,
                                    accum)) {
                    std::lock_guard<std::mutex> lock(state.mutex);
                    state.luma.pixels      = luma;
                    state.luma.width       = config.lumaWidth;
                    state.luma.height      = config.lumaHeight;
                    state.luma.seq         = ++state.luma_seq;
                    state.luma.timestampUs = frame_at;
                }
            }

            // RGB565 for the RADAR face detector / camera view, only for a frame a consumer asked for
            if (_rgb_requested.exchange(false)) {
                int64_t rgb_start = now_us();
                if (publish_rgb(frame, width, height, fmt, config.rgbDownscale, frame_at)) {
                    window_rgbs++;
                    rgb_ms = ema(rgb_ms, (now_us() - rgb_start) / 1000.0f);
                } else {
                    _rgb_requested.store(true);  // retry with the next frame
                }
            }

            // JPEG, only while somebody wants it, paced to maxFps with a token bucket so a 15 fps target on a
            // 20 fps sensor really yields 15 fps instead of every other frame
            jpeg_pixel_format_t jpeg_format;
            if (_jpeg_consumers.load() <= 0 || !to_jpeg_format(fmt, jpeg_format)) {
                next_jpeg_due_us = 0;
                return;
            }
            const int64_t interval_us = config.maxFps > 0 ? 1000000 / config.maxFps : 0;
            if (frame_at < next_jpeg_due_us) {
                return;
            }
            next_jpeg_due_us = std::max(next_jpeg_due_us + interval_us, frame_at);
            if (!encoder.prepare(width, height, jpeg_format, _jpeg_quality.load())) {
                return;
            }
            int64_t encode_start = now_us();
            int length           = encoder.encode(frame, size);
            int64_t encode_end   = now_us();
            if (length <= 0) {
                return;
            }
            auto data =
                std::make_shared<const std::vector<uint8_t>>(encoder.output(), encoder.output() + length);
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                state.jpeg.data        = std::move(data);
                state.jpeg.seq         = ++state.jpeg_seq;
                state.jpeg.timestampUs = frame_at;
            }
            state.cv.notify_all();
            window_jpegs++;
            last_jpeg_bytes = static_cast<uint32_t>(length);
            encode_ms       = ema(encode_ms, (encode_end - encode_start) / 1000.0f);
        });

        if (!visited) {
            if (failure_count++ % 50 == 0) {
                mclog::tagWarn(kTag, "capture failed ({} times)", failure_count);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        failure_count = 0;
        window_captures++;
        if (frame_at > 0) {
            capture_ms = ema(capture_ms, (frame_at - wait_start) / 1000.0f);
        }

        int64_t now = now_us();
        if (now - window_start_us >= kStatsWindowUs) {
            float seconds   = (now - window_start_us) / 1000000.0f;
            capture_fps     = window_captures / seconds;
            jpeg_fps        = window_jpegs / seconds;
            rgb_fps         = window_rgbs / seconds;
            window_captures = 0;
            window_jpegs    = 0;
            window_rgbs     = 0;
            window_start_us = now;
            std::lock_guard<std::mutex> lock(state.mutex);
            state.stats.captureFps = capture_fps;
            state.stats.jpegFps    = jpeg_fps;
            state.stats.captureMs  = capture_ms;
            state.stats.encodeMs   = encode_ms;
            state.stats.jpegBytes  = last_jpeg_bytes;
            state.stats.rgbFps     = rgb_fps;
            state.stats.rgbMs      = rgb_ms;
        }
        if (now - last_log_us >= kStatsLogPeriodUs) {
            last_log_us = now;
            mclog::tagInfo(kTag, "capture {:.1f} fps ({:.1f} ms wait), jpeg {:.1f} fps ({:.1f} ms, {} B), consumers {}",
                           capture_fps, capture_ms, jpeg_fps, encode_ms, last_jpeg_bytes, _jpeg_consumers.load());
            if (rgb_fps > 0.0f) {
                mclog::tagInfo(kTag, "rgb565 {:.1f} fps ({:.1f} ms per frame)", rgb_fps, rgb_ms);
            }
        }

        // Always leave the idle task of this core a slice: frames arrive every 50 ms and the work takes ~25 ms,
        // but two already filled buffers would otherwise be processed back to back
        vTaskDelay(1);
    }
}

void capture_task(void* /*param*/)
{
    mclog::tagInfo(kTag, "capture task started");
    run_capture_loop(hal_bridge::board_get_camera());

    {
        std::lock_guard<std::mutex> lock(shared().mutex);
        shared().stats = GrabberStats{};
        shared().rgb   = RgbFrame{};  // release the last RGB buffer (consumers keep their own frames alive)
    }
    mclog::tagInfo(kTag, "capture task stopped");
    _running.store(false);
    vTaskDelete(nullptr);
}

}  // namespace

FrameGrabber& FrameGrabber::instance()
{
    static FrameGrabber grabber;
    return grabber;
}

bool FrameGrabber::start(const GrabberConfig& config)
{
    std::lock_guard<std::mutex> lifecycle(_lifecycle_mutex);

    {
        std::lock_guard<std::mutex> lock(shared().mutex);
        shared().config = config;
    }
    setJpegQuality(config.jpegQuality);

    if (_running.load()) {
        return true;
    }

    auto* camera = hal_bridge::board_get_camera();
    if (camera == nullptr || !camera->IsStreaming()) {
        // The camera object exists even when the sensor failed to initialize
        mclog::tagError(kTag, "camera unavailable ({})", camera == nullptr ? "no camera" : "stream not running");
        return false;
    }

    _stop_requested.store(false);
    _running.store(true);
    if (xTaskCreatePinnedToCore(capture_task, "frame_grabber", kTaskStackSize, nullptr, kTaskPriority, nullptr,
                                kTaskCore) != pdPASS) {
        mclog::tagError(kTag, "failed to create capture task");
        _running.store(false);
        return false;
    }
    mclog::tagInfo(kTag, "started: jpeg q{}, max {} fps, luma {}x{}", config.jpegQuality, config.maxFps,
                   config.lumaWidth, config.lumaHeight);
    return true;
}

void FrameGrabber::stop()
{
    std::lock_guard<std::mutex> lifecycle(_lifecycle_mutex);
    if (!_running.load()) {
        return;
    }

    _stop_requested.store(true);

    // The task notices the request after the current frame (<= 50 ms at 20 fps)
    uint32_t waited = 0;
    while (_running.load() && waited < kStopTimeoutMs) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
    if (_running.load()) {
        // Leave _stop_requested set so the task still exits whenever the camera wakes it up
        mclog::tagError(kTag, "capture task did not stop within {} ms", kStopTimeoutMs);
        return;
    }
    _stop_requested.store(false);
}

bool FrameGrabber::isRunning() const
{
    return _running.load();
}

void FrameGrabber::addJpegConsumer()
{
    _jpeg_consumers.fetch_add(1);
}

void FrameGrabber::removeJpegConsumer()
{
    int current = _jpeg_consumers.load();
    while (current > 0 && !_jpeg_consumers.compare_exchange_weak(current, current - 1)) {
    }
}

void FrameGrabber::setJpegQuality(uint8_t quality)
{
    _jpeg_quality.store(std::clamp<uint8_t>(quality, 1, 100));
}

bool FrameGrabber::waitJpeg(uint32_t afterSeq, JpegFrame& out, uint32_t timeoutMs)
{
    auto& state = shared();
    std::unique_lock<std::mutex> lock(state.mutex);
    // Only a new frame ends the wait early: a stopped grabber makes the caller wait the full timeout, so a
    // consumer that loops on waitJpeg() can never spin
    auto has_new_frame = [&]() { return state.jpeg.data && state.jpeg.seq != afterSeq; };
    if (!state.cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), has_new_frame)) {
        return false;
    }
    out = state.jpeg;
    return true;
}

bool FrameGrabber::latestLuma(uint32_t afterSeq, LumaFrame& out)
{
    auto& state = shared();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.luma.pixels.empty() || state.luma.seq == afterSeq) {
        return false;
    }
    out = state.luma;
    return true;
}

void FrameGrabber::requestRgb()
{
    _rgb_requested.store(true);
}

bool FrameGrabber::waitRgb(uint32_t afterSeq, RgbFrame& out, uint32_t timeoutMs)
{
    auto& state = shared();
    std::unique_lock<std::mutex> lock(state.mutex);
    auto has_new_frame = [&]() { return state.rgb.pixels && state.rgb.seq != afterSeq; };
    if (!has_new_frame()) {
        _rgb_requested.store(true);
        if (!state.rgb_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), has_new_frame)) {
            return false;
        }
    }
    out = state.rgb;
    return true;
}

bool FrameGrabber::latestRgb(uint32_t afterSeq, RgbFrame& out)
{
    auto& state = shared();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.rgb.pixels || state.rgb.seq == afterSeq) {
        return false;
    }
    out = state.rgb;
    return true;
}

GrabberStats FrameGrabber::stats() const
{
    std::lock_guard<std::mutex> lock(shared().mutex);
    return shared().stats;
}

}  // namespace stackpet
