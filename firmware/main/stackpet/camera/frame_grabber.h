/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace stackpet {

struct GrabberConfig {
    uint8_t jpegQuality = 50;   // 1-100
    uint8_t maxFps      = 15;   // JPEG rate upper bound (frames are still dequeued at the sensor rate to stay fresh)
    uint16_t lumaWidth  = 40;   // downscaled luma grid for vision; the sensor delivers 320x240 YUYV.
    uint16_t lumaHeight = 30;   // 0 x 0 disables the luma output (streaming-only users)
    // RGB565 output (RADAR face detection and camera view): sensor resolution divided by this factor, 1 = 320x240,
    // 2 = 160x120 (2x2 box filter). Frames are only converted on request, see FrameGrabber::requestRgb().
    uint8_t rgbDownscale = 1;
};

struct JpegFrame {
    std::shared_ptr<const std::vector<uint8_t>> data;  // a complete JPEG file
    uint32_t seq        = 0;                            // increments for every encoded frame
    int64_t timestampUs = 0;                            // esp_timer time of the capture
};

struct LumaFrame {
    std::vector<uint8_t> pixels;  // width * height, row-major, 0-255
    uint16_t width      = 0;
    uint16_t height     = 0;
    uint32_t seq        = 0;      // increments for every captured frame
    int64_t timestampUs = 0;
};

/**
 * RGB565 frame in LVGL's native byte order (= esp-dl DL_IMAGE_PIX_TYPE_RGB565LE). The pixel buffer is shared and
 * immutable: hold the frame for as long as the pixels are used (an lv_image may point at them directly).
 */
struct RgbFrame {
    std::shared_ptr<const uint16_t> pixels;  // width * height values, row-major
    uint16_t width      = 0;
    uint16_t height     = 0;
    uint32_t seq        = 0;  // increments for every converted frame
    int64_t timestampUs = 0;  // esp_timer time of the capture
};

struct GrabberStats {
    float captureFps   = 0;  // captured frames per second (smoothed)
    float jpegFps      = 0;  // encoded JPEG frames per second (smoothed)
    float captureMs    = 0;  // average time waiting for the sensor to deliver a frame
    float encodeMs     = 0;  // average JPEG encode time
    uint32_t jpegBytes = 0;  // size of the latest JPEG
    float rgbFps       = 0;  // converted RGB565 frames per second
    float rgbMs        = 0;  // average RGB565 conversion time
};

/**
 * @brief Background camera pipeline shared by the LAN streamer, the official websocket streamer and the games.
 *
 * A single task owns the camera: capture -> (optional) downscaled luma for vision -> (optional) JPEG encode.
 * Consumers never touch the camera directly; they read the latest published frame (old frames are dropped),
 * so a slow network client can never stall capture or another consumer. All methods are thread-safe.
 */
class FrameGrabber {
public:
    static FrameGrabber& instance();

    // Start the capture task (idempotent, a running grabber adopts the new config). Returns false if there is no
    // camera or its V4L2 stream is not running.
    bool start(const GrabberConfig& config = {});
    // Stop the capture task and wait for it to exit (idempotent).
    void stop();
    bool isRunning() const;

    // JPEG encoding only runs while at least one consumer is registered (saves CPU for vision-only users).
    void addJpegConsumer();
    void removeJpegConsumer();
    void setJpegQuality(uint8_t quality);

    // Wait until a JPEG newer than afterSeq is available, or until timeout. Returns false on timeout. It never
    // returns early because the grabber is stopped, so a consumer loop cannot spin (timeoutMs = 0 polls).
    bool waitJpeg(uint32_t afterSeq, JpegFrame& out, uint32_t timeoutMs);
    // Copy the latest luma frame if its seq is newer than afterSeq. Returns false if nothing newer.
    bool latestLuma(uint32_t afterSeq, LumaFrame& out);

    // RGB565 output, converted on demand: the capture task converts the next sensor frame only after a consumer
    // asked for one, so the cost follows the consumers' pace and is zero while nobody asks (REMOTE / FREEZE never
    // do). requestRgb() asks for the next frame without waiting (e.g. to prefetch while processing one).
    void requestRgb();
    // Wait until an RGB frame newer than afterSeq is available (requesting one if there is none yet), or until
    // timeout. Like waitJpeg() it never returns early because the grabber is stopped.
    bool waitRgb(uint32_t afterSeq, RgbFrame& out, uint32_t timeoutMs);
    // Copy the latest RGB frame if its seq is newer than afterSeq (does not request a new one).
    bool latestRgb(uint32_t afterSeq, RgbFrame& out);

    GrabberStats stats() const;
};

namespace pixel {

// Full-range BT.601 YUV to RGB565, with the coefficients esp-dl uses (Q14), so the detector sees the same colors.
inline uint16_t yuvToRgb565(int y, int u, int v)
{
    u -= 128;
    v -= 128;
    int r = y + ((22987 * v + 8192) >> 14);
    int g = y + ((-5636 * u - 11698 * v + 8192) >> 14);
    int b = y + ((29049 * u + 8192) >> 14);
    r     = r < 0 ? 0 : (r > 255 ? 255 : r);
    g     = g < 0 ? 0 : (g > 255 ? 255 : g);
    b     = b < 0 ? 0 : (b > 255 ? 255 : b);
    return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/**
 * Convert a YUYV (Y0 U Y1 V) image to RGB565. downscale 2 averages 2x2 blocks (luma of 4 pixels, chroma of the
 * two rows). dst holds (width / downscale) * (height / downscale) values. Pure function, host tested.
 */
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("O2")))
#endif
inline void yuyvToRgb565(const uint8_t* src, int width, int height, int downscale, uint16_t* dst)
{
    const int stride = width * 2;
    if (downscale == 2) {
        for (int y = 0; y + 1 < height; y += 2) {
            const uint8_t* row0 = src + static_cast<size_t>(y) * stride;
            const uint8_t* row1 = row0 + stride;
            for (int x = 0; x + 1 < width; x += 2) {
                const uint8_t* a = row0 + 2 * x;
                const uint8_t* b = row1 + 2 * x;
                int luma         = (a[0] + a[2] + b[0] + b[2] + 2) >> 2;
                int u            = (a[1] + b[1] + 1) >> 1;
                int v            = (a[3] + b[3] + 1) >> 1;
                *dst++           = yuvToRgb565(luma, u, v);
            }
        }
        return;
    }
    const size_t pairs = static_cast<size_t>(width / 2) * height;
    for (size_t i = 0; i < pairs; i++, src += 4) {
        const int u  = src[1] - 128;
        const int v  = src[3] - 128;
        const int dr = (22987 * v + 8192) >> 14;
        const int dg = (-5636 * u - 11698 * v + 8192) >> 14;
        const int db = (29049 * u + 8192) >> 14;
        for (int k = 0; k < 2; k++) {
            const int luma = src[2 * k];
            int r          = luma + dr;
            int g          = luma + dg;
            int b          = luma + db;
            r              = r < 0 ? 0 : (r > 255 ? 255 : r);
            g              = g < 0 ? 0 : (g > 255 ? 255 : g);
            b              = b < 0 ? 0 : (b > 255 ? 255 : b);
            *dst++         = static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
}

}  // namespace pixel

}  // namespace stackpet
