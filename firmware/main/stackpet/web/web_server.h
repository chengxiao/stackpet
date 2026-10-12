/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace stackpet::web {

/**
 * LAN remote control server (no cloud):
 *   port 80: GET /                -> embedded control page (stackpet/web/index.html), no token needed
 *            GET /api/status      -> JSON status (see StatusSnapshot + camera stats)
 *            GET /snapshot.jpg    -> latest frame
 *            POST /api/photo      -> {"id":N}: countdown + fill light on the robot, then GET /photo.jpg?id=N
 *            POST /api/face?style=0|1|2 -> face style (stackpet::face::Style), the app rebuilds its avatar
 *            POST /api/...        -> commands, queued for the app main loop (see Command)
 *   port 81: GET /stream?k=TOKEN  -> MJPEG (multipart/x-mixed-replace) from FrameGrabber, up to 2 viewers
 *
 * Security: everything except "/", "/index.html" and "/favicon.ico" requires the access token set with
 * setAccessToken() (header "X-StackPet-Key", or query "k" for the <img> stream), otherwise 403. The token is
 * random per app session and only travels in the QR code shown on the robot, so a device on the same Wi-Fi
 * cannot drive the robot or watch the camera without having scanned it. No CORS headers are sent.
 *
 * The stream lives on its own server instance and every viewer is served by its own task (async handler),
 * because esp_http_server handles requests one at a time.
 * FrameGrabber must be started by the app; the server only registers itself as a JPEG consumer while streaming.
 */
enum class CommandType : uint8_t {
    Look,       // x: -1 (left) .. 1 (right) as seen in the camera image, y: -1 (down) .. 1 (up); a: speed 0-1000
    Home,       // a: speed
    Emotion,    // a: avatar::Emotion value
    Decorator,  // text: "heart" | "angry" | "sweat" | "shy" | "dizzy"
    Rgb,        // a, b, c: r, g, b
    Action,     // text: "nod" | "shake" | "spin" | "dance"
    Speak,      // text: UTF-8 text shown in the speech bubble (empty = clear)
    Sfx,        // a: stackpet::Sfx value
    Volume,     // a: speaker volume 0-100 (persisted, so apply it from the main loop)
    Photo,      // a: photo id; take the photo (countdown + fill light), then publishPhoto(id, jpeg)
    Face,       // a: stackpet::face::Style value (validated); setStyle() + avatar rebuild in the main loop
#if STACKPET_CAL
    DebugSpin,  // test builds only: a: wheel mode velocity (-1000..1000), b: duration ms; logs the raw yaw readings
#endif
};

struct Command {
    CommandType type = CommandType::Home;
    float x          = 0.0f;
    float y          = 0.0f;
    int a            = 0;
    int b            = 0;
    int c            = 0;
    std::string text;
};

struct StatusSnapshot {
    int batteryLevel = -1;
    bool charging    = false;
    int yaw          = 0;
    int pitch        = 0;
    int volume       = -1;
    int faceStyle    = -1;  // stackpet::face::Style, served as "face_style"
    std::string ip;  // filled by the app main loop (WifiManager is not read from the HTTP tasks)
};

class WebServer {
public:
    static WebServer& instance();

    bool start();  // idempotent; requires the network to be connected
    void stop();   // idempotent, waits for the stream viewers to leave
    bool isRunning() const;

    // Required for every request except the page itself. An empty token rejects everything.
    void setAccessToken(const std::string& token);

    // Thread-safe command queue, drained by the app main loop (which owns LVGL / motion).
    // Consecutive Look commands are coalesced so a joystick can never build a backlog.
    bool popCommand(Command& out);
    // Updated by the app main loop, served by /api/status.
    void setStatus(const StatusSnapshot& status);
    // Result of a Photo command, served by /photo.jpg?id=<id>.
    void publishPhoto(uint32_t id, std::shared_ptr<const std::vector<uint8_t>> jpeg);

    int streamClientCount() const;
    uint32_t lastRequestMs() const;  // GetHAL().millis() of the last authorized HTTP request, 0 if none
};

}  // namespace stackpet::web
