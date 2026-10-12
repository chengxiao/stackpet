#pragma once
#include "sdkconfig.h"

#ifndef CONFIG_IDF_TARGET_ESP32
#include <lvgl.h>
#include <functional>
#include <thread>
#include <memory>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "camera.h"
#include "jpg/image_to_jpeg.h"
#include "esp_video_init.h"

struct JpegChunk {
    uint8_t* data;
    size_t len;
};

class StackChanCamera : public Camera {
private:
    struct FrameBuffer {
        uint8_t* data         = nullptr;
        size_t len            = 0;
        uint16_t width        = 0;
        uint16_t height       = 0;
        v4l2_pix_fmt_t format = 0;
    } frame_;
    // Allocated size of frame_.data when it was allocated by StreamCaptures() (64-byte aligned, reused between
    // frames). 0 means "unknown / not reusable" and forces a new allocation.
    size_t frame_capacity_        = 0;
    v4l2_pix_fmt_t sensor_format_ = 0;
#ifdef CONFIG_XIAOZHI_ENABLE_ROTATE_CAMERA_IMAGE
    uint16_t sensor_width_  = 0;
    uint16_t sensor_height_ = 0;
#endif  // CONFIG_XIAOZHI_ENABLE_ROTATE_CAMERA_IMAGE
    int video_fd_      = -1;
    bool streaming_on_ = false;
    struct MmapBuffer {
        void* start   = nullptr;
        size_t length = 0;
    };
    std::vector<MmapBuffer> mmap_buffers_;
    std::string explain_url_;
    std::string explain_token_;
    std::thread encoder_thread_;

public:
    StackChanCamera(const esp_video_init_config_t& config);
    ~StackChanCamera();

    virtual void SetExplainUrl(const std::string& url, const std::string& token);
    virtual bool Capture() override;
    // Grab the next frame into an internal buffer (GetFrameData() & co). The buffer is reused between calls and is
    // 64-byte aligned, so it can be fed to esp_new_jpeg directly. Must not be called concurrently with Capture().
    bool StreamCaptures();

    // Zero-copy alternative to StreamCaptures(): dequeue the next frame, let `visitor` read the driver buffer in
    // place, then requeue it. `data` is only valid during the call (cache-line aligned, YUV422P is reported as
    // YUYV like StreamCaptures() does). Frames that need a conversion first fall back to the copying path.
    // Must not be called concurrently with Capture() / StreamCaptures().
    using FrameVisitor =
        std::function<void(const uint8_t* data, size_t size, uint16_t width, uint16_t height, v4l2_pix_fmt_t format)>;
    bool VisitFrame(const FrameVisitor& visitor);

    // True once the V4L2 stream is running (a camera object can exist without a working sensor)
    bool IsStreaming() const
    {
        return streaming_on_ && video_fd_ >= 0;
    }

    // 翻转控制函数
    virtual bool SetHMirror(bool enabled) override;
    virtual bool SetVFlip(bool enabled) override;
    virtual std::string Explain(const std::string& question);

    const uint8_t* GetFrameData()
    {
        return frame_.data;
    }
    size_t GetFrameSize()
    {
        return frame_.len;
    }
    int GetFrameWidth()
    {
        return frame_.width;
    }
    int GetFrameHeight()
    {
        return frame_.height;
    }
    int GetFrameFormat()
    {
        return frame_.format;
    }
};

#endif  // ndef CONFIG_IDF_TARGET_ESP32
