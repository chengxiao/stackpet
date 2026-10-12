/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "web_server.h"
#include "web_utils.h"
#include <stackpet/camera/frame_grabber.h>
#include <stackpet/face/face.h>
#if STACKPET_CAL
#include <stackpet/vision/face_detector.h>
#endif
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <mooncake_log.h>
#include <atomic>
#include <deque>
#include <mutex>

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

namespace stackpet::web {

namespace {

const char* kTag = "WebServer";

// Socket budget (CONFIG_LWIP_MAX_SOCKETS = 16): every httpd instance needs max_open_sockets + 3 sockets
// (listen, control and one internal), so control 4 + 3 and stream 3 + 3 = 13, leaving 3 for SNTP / DNS.
constexpr uint16_t kControlPort        = 80;
constexpr uint16_t kControlCtrlPort    = 32768;
constexpr uint16_t kControlMaxSockets  = 4;
constexpr size_t kControlStackSize     = 6144;
constexpr uint16_t kStreamPort         = 81;
constexpr uint16_t kStreamCtrlPort     = 32769;
constexpr int kMaxStreamClients        = 2;
constexpr size_t kStreamServerStack    = 4096;
constexpr uint32_t kStreamTaskStack    = 4096;
constexpr UBaseType_t kStreamTaskPrio  = 4;
constexpr uint16_t kStreamSendTimeoutS = 3;
constexpr uint32_t kStopTimeoutMs      = (kStreamSendTimeoutS + 1) * 1000;
constexpr size_t kMaxQueuedCommands    = 24;
constexpr size_t kMaxBodySize          = 1024;
constexpr size_t kMaxSpeakChars        = 60;
constexpr size_t kMaxTokenLength       = 32;
constexpr int64_t kSnapshotMaxAgeUs    = 400000;
constexpr const char* kKeyHeader       = "X-StackPet-Key";
constexpr int kFaceStyleCount          = static_cast<int>(face::Style::Count);

struct State {
    std::mutex lifecycle;
    httpd_handle_t control = nullptr;
    httpd_handle_t stream  = nullptr;
    std::atomic<bool> running{false};
    std::atomic<bool> stopping{false};

    std::mutex mutex;  // commands, status, token, photo
    std::deque<Command> commands;
    StatusSnapshot status;
    std::string token;
    uint32_t next_photo_id = 0;
    uint32_t photo_id      = 0;
    std::shared_ptr<const std::vector<uint8_t>> photo;

    std::atomic<int> stream_clients{0};
    std::atomic<uint32_t> last_request_ms{0};
    std::atomic<uint32_t> stream_fps_x10{0};
};

State& state()
{
    static State instance;
    return instance;
}

uint32_t millis_now()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

std::string to_lower(std::string text)
{
    for (auto& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

std::string get_query(httpd_req_t* req)
{
    size_t len = httpd_req_get_url_query_len(req);
    if (len == 0) {
        return {};
    }
    std::string query(len + 1, '\0');
    if (httpd_req_get_url_query_str(req, query.data(), query.size()) != ESP_OK) {
        return {};
    }
    query.resize(len);
    return query;
}

esp_err_t send_json(httpd_req_t* req, const char* status, const std::string& body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body.data(), body.size());
}

esp_err_t send_ok(httpd_req_t* req)
{
    return send_json(req, "200 OK", "{\"ok\":true}");
}

esp_err_t send_error(httpd_req_t* req, const char* status, const char* message)
{
    return send_json(req, status, std::string("{\"ok\":false,\"error\":\"") + utils::jsonEscape(message) + "\"}");
}

/**
 * Checks the access token (header, or query "k" for the <img> stream that cannot send headers).
 * Sends the 403 itself; handlers return ESP_OK when this returns false.
 */
bool authorize(httpd_req_t* req)
{
    std::string provided;
    size_t header_len = httpd_req_get_hdr_value_len(req, kKeyHeader);
    if (header_len > 0 && header_len <= kMaxTokenLength) {
        char buf[kMaxTokenLength + 1] = {};
        if (httpd_req_get_hdr_value_str(req, kKeyHeader, buf, sizeof(buf)) == ESP_OK) {
            provided = buf;
        }
    }
    if (provided.empty()) {
        provided = utils::queryValue(get_query(req), "k").value_or("");
    }

    std::string expected;
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        expected = state().token;
    }
    if (!utils::tokenMatches(expected, provided)) {
        send_error(req, "403 Forbidden", "bad key");
        return false;
    }
    state().last_request_ms.store(millis_now());
    return true;
}

void push_command(Command&& command)
{
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (command.type == CommandType::Look && !s.commands.empty() && s.commands.back().type == CommandType::Look) {
        s.commands.back() = std::move(command);
        return;
    }
    if (s.commands.size() >= kMaxQueuedCommands) {
        s.commands.pop_front();
    }
    s.commands.push_back(std::move(command));
}

/* ------------------------------ Control server ----------------------------- */

esp_err_t index_handler(httpd_req_t* req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    // EMBED_TXTFILES appends a null terminator
    const size_t length = static_cast<size_t>(index_html_end - index_html_start) - 1;
    return httpd_resp_send(req, index_html_start, length);
}

esp_err_t favicon_handler(httpd_req_t* req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, nullptr, 0);
}

esp_err_t status_handler(httpd_req_t* req)
{
    if (!authorize(req)) {
        return ESP_OK;
    }
    StatusSnapshot status;
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        status = state().status;
    }
    auto& grabber = FrameGrabber::instance();
    auto stats    = grabber.stats();

    // fmt instead of printf: the firmware's newlib-nano printf has no 64-bit integer support
    auto body = fmt::format(
        "{{\"battery\":{},\"charging\":{},\"yaw\":{},\"pitch\":{},\"volume\":{},\"face_style\":{},"
        "\"camera\":{},\"fps\":{:.1f},"
        "\"captureFps\":{:.1f},\"encodeMs\":{:.1f},\"jpegBytes\":{},\"clients\":{},\"maxClients\":{},"
        "\"streamFps\":{:.1f},\"ip\":\"{}\",\"uptime\":{},\"heapInternal\":{},\"heapPsram\":{}}}",
        status.batteryLevel, status.charging, status.yaw, status.pitch, status.volume, status.faceStyle,
        grabber.isRunning(),
        stats.jpegFps, stats.captureFps, stats.encodeMs, stats.jpegBytes, state().stream_clients.load(),
        kMaxStreamClients, state().stream_fps_x10.load() / 10.0f, utils::jsonEscape(status.ip),
        esp_timer_get_time() / 1000000, heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return send_json(req, "200 OK", body);
}

esp_err_t send_jpeg(httpd_req_t* req, const std::vector<uint8_t>& jpeg, const char* filename)
{
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", filename);
    return httpd_resp_send(req, reinterpret_cast<const char*>(jpeg.data()), jpeg.size());
}

esp_err_t snapshot_handler(httpd_req_t* req)
{
    if (!authorize(req)) {
        return ESP_OK;
    }
    auto& grabber = FrameGrabber::instance();
    if (!grabber.isRunning()) {
        return send_error(req, "503 Service Unavailable", "camera unavailable");
    }

    grabber.addJpegConsumer();
    JpegFrame frame;
    bool ok = grabber.waitJpeg(0, frame, 0);
    if (!ok || esp_timer_get_time() - frame.timestampUs > kSnapshotMaxAgeUs) {
        // No JPEG yet, or the latest one is from before anybody was watching: wait for a fresh one
        ok = grabber.waitJpeg(frame.seq, frame, 1500);
    }
    grabber.removeJpegConsumer();
    if (!ok || !frame.data) {
        return send_error(req, "503 Service Unavailable", "no frame");
    }
    return send_jpeg(req, *frame.data, "inline; filename=\"stackchan.jpg\"");
}

#if STACKPET_FD_BENCH
// Test builds only: a fresh RGB565 frame exactly as the face detector gets it (raw little-endian pixels, the size in
// the X-Width / X-Height headers), to compare what the detector sees with /snapshot.jpg
esp_err_t debug_rgb_handler(httpd_req_t* req)
{
    if (!authorize(req)) {
        return ESP_OK;
    }
    auto& grabber = FrameGrabber::instance();
    if (!grabber.isRunning()) {
        return send_error(req, "503 Service Unavailable", "camera unavailable");
    }
    RgbFrame latest;
    grabber.latestRgb(0, latest);
    RgbFrame frame;
    if (!grabber.waitRgb(latest.seq, frame, 1500) || !frame.pixels) {
        return send_error(req, "503 Service Unavailable", "no frame");
    }
    const std::string width  = std::to_string(frame.width);
    const std::string height = std::to_string(frame.height);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "X-Width", width.c_str());
    httpd_resp_set_hdr(req, "X-Height", height.c_str());
    return httpd_resp_send(req, reinterpret_cast<const char*>(frame.pixels.get()),
                           static_cast<ssize_t>(frame.width) * frame.height * sizeof(uint16_t));
}
#endif

#if STACKPET_CAL
// Test builds only (aim / rotation calibration): the latest face detector result as JSON, capture time in esp_timer ms
esp_err_t debug_faces_handler(httpd_req_t* req)
{
    if (!authorize(req)) {
        return ESP_OK;
    }
    auto& detector = stackpet::vision::FaceDetector::instance();
    stackpet::vision::FaceFrame frame;
    const bool have = detector.latest(0, frame);
    const auto stats = detector.stats();
    std::string json = "{\"ready\":" + std::string(stats.ready ? "true" : "false") +
                       ",\"fps\":" + std::to_string(stats.fps) + ",\"seq\":" + std::to_string(have ? frame.seq : 0) +
                       ",\"captureMs\":" + std::to_string(have ? frame.frame.timestampUs / 1000 : 0) +
                       ",\"nowMs\":" + std::to_string(esp_timer_get_time() / 1000) + ",\"faces\":[";
    if (have) {
        for (size_t i = 0; i < frame.faces.size(); i++) {
            const auto& f = frame.faces[i];
            json += (i ? "," : "") + std::string("[") + std::to_string(f.cx) + "," + std::to_string(f.cy) + "," +
                    std::to_string(f.w) + "," + std::to_string(f.h) + "," + std::to_string(f.score) + "]";
        }
    }
    json += "]}";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json.data(), static_cast<ssize_t>(json.size()));
}

// Test builds only: wheel mode rotation at v (-1000..1000) for ms, the raw yaw readings go to the log ("SPINLOG")
esp_err_t debug_spin_handler(httpd_req_t* req)
{
    if (!authorize(req)) {
        return ESP_OK;
    }
    auto query = get_query(req);
    Command command;
    command.type = CommandType::DebugSpin;
    command.a    = utils::parseInt(utils::queryValue(query, "v"), 0, -1000, 1000);
    command.b    = utils::parseInt(utils::queryValue(query, "ms"), 1000, 0, 20000);
    push_command(std::move(command));
    return send_ok(req);
}
#endif

esp_err_t photo_request_handler(httpd_req_t* req)
{
    if (!authorize(req)) {
        return ESP_OK;
    }
    if (!FrameGrabber::instance().isRunning()) {
        return send_error(req, "503 Service Unavailable", "camera unavailable");
    }
    uint32_t id;
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        id = ++state().next_photo_id;
    }
    Command command;
    command.type = CommandType::Photo;
    command.a    = static_cast<int>(id);
    push_command(std::move(command));
    return send_json(req, "200 OK", fmt::format("{{\"ok\":true,\"id\":{}}}", id));
}

esp_err_t photo_get_handler(httpd_req_t* req)
{
    if (!authorize(req)) {
        return ESP_OK;
    }
    auto id = static_cast<uint32_t>(utils::parseInt(utils::queryValue(get_query(req), "id"), 0, 0, INT32_MAX));
    std::shared_ptr<const std::vector<uint8_t>> photo;
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        if (id != 0 && id == state().photo_id) {
            photo = state().photo;
        }
    }
    if (!photo) {
        // Still counting down (the page polls)
        return send_error(req, "404 Not Found", "not ready");
    }
    return send_jpeg(req, *photo, "inline; filename=\"stackchan-photo.jpg\"");
}

esp_err_t look_handler(httpd_req_t* req)
{
    if (!authorize(req)) {
        return ESP_OK;
    }
    auto query = get_query(req);
    Command command;
    command.type = CommandType::Look;
    command.x    = utils::parseFloat(utils::queryValue(query, "x"), 0.0f, -1.0f, 1.0f);
    command.y    = utils::parseFloat(utils::queryValue(query, "y"), 0.0f, -1.0f, 1.0f);
    command.a    = utils::parseInt(utils::queryValue(query, "speed"), 700, 0, 1000);
    push_command(std::move(command));
    return send_ok(req);
}

esp_err_t home_handler(httpd_req_t* req)
{
    if (!authorize(req)) {
        return ESP_OK;
    }
    Command command;
    command.type = CommandType::Home;
    command.a    = utils::parseInt(utils::queryValue(get_query(req), "speed"), 500, 0, 1000);
    push_command(std::move(command));
    return send_ok(req);
}

esp_err_t emotion_handler(httpd_req_t* req)
{
    if (!authorize(req)) {
        return ESP_OK;
    }
    int emotion = utils::emotionFromName(utils::queryValue(get_query(req), "e").value_or(""));
    if (emotion < 0) {
        return send_error(req, "400 Bad Request", "unknown emotion");
    }
    Command command;
    command.type = CommandType::Emotion;
    command.a    = emotion;
    push_command(std::move(command));
    return send_ok(req);
}

esp_err_t decorator_handler(httpd_req_t* req)
{
    if (!authorize(req)) {
        return ESP_OK;
    }
    auto name = to_lower(utils::queryValue(get_query(req), "d").value_or(""));
    if (!utils::isDecoratorName(name)) {
        return send_error(req, "400 Bad Request", "unknown decorator");
    }
    Command command;
    command.type = CommandType::Decorator;
    command.text = std::move(name);
    push_command(std::move(command));
    return send_ok(req);
}

esp_err_t rgb_handler(httpd_req_t* req)
{
    if (!authorize(req)) {
        return ESP_OK;
    }
    auto query = get_query(req);
    Command command;
    command.type = CommandType::Rgb;
    command.a    = utils::parseInt(utils::queryValue(query, "r"), 0, 0, 255);
    command.b    = utils::parseInt(utils::queryValue(query, "g"), 0, 0, 255);
    command.c    = utils::parseInt(utils::queryValue(query, "b"), 0, 0, 255);
    push_command(std::move(command));
    return send_ok(req);
}

esp_err_t action_handler(httpd_req_t* req)
{
    if (!authorize(req)) {
        return ESP_OK;
    }
    auto name = to_lower(utils::queryValue(get_query(req), "a").value_or(""));
    if (!utils::isActionName(name)) {
        return send_error(req, "400 Bad Request", "unknown action");
    }
    Command command;
    command.type = CommandType::Action;
    command.text = std::move(name);
    push_command(std::move(command));
    return send_ok(req);
}

esp_err_t speak_handler(httpd_req_t* req)
{
    if (!authorize(req)) {
        return ESP_OK;
    }
    if (req->content_len > kMaxBodySize) {
        return send_error(req, "413 Payload Too Large", "text too long");
    }

    std::string body(req->content_len, '\0');
    size_t received = 0;
    int timeouts    = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body.data() + received, req->content_len - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) {
            continue;
        }
        if (ret <= 0) {
            return ESP_FAIL;
        }
        received += static_cast<size_t>(ret);
    }
    if (body.empty()) {
        body = utils::queryValue(get_query(req), "t").value_or("");
    }

    Command command;
    command.type = CommandType::Speak;
    command.text = utils::sanitizeUtf8(body, kMaxSpeakChars);
    push_command(std::move(command));
    return send_ok(req);
}

esp_err_t sfx_handler(httpd_req_t* req)
{
    if (!authorize(req)) {
        return ESP_OK;
    }
    int sfx = utils::sfxFromName(utils::queryValue(get_query(req), "s").value_or(""));
    if (sfx < 0) {
        return send_error(req, "400 Bad Request", "unknown sound");
    }
    Command command;
    command.type = CommandType::Sfx;
    command.a    = sfx;
    push_command(std::move(command));
    return send_ok(req);
}

esp_err_t volume_handler(httpd_req_t* req)
{
    if (!authorize(req)) {
        return ESP_OK;
    }
    auto value = utils::queryValue(get_query(req), "v");
    if (!value) {
        return send_error(req, "400 Bad Request", "missing v");
    }
    Command command;
    command.type = CommandType::Volume;
    command.a    = utils::parseInt(value, 50, 0, 100);
    push_command(std::move(command));
    return send_ok(req);
}

esp_err_t face_handler(httpd_req_t* req)
{
    if (!authorize(req)) {
        return ESP_OK;
    }
    // Strict: exactly "0", "1" or "2" (stackpet::face::Style, Count = 3)
    int style = utils::parseStrictIndex(utils::queryValue(get_query(req), "style"), kFaceStyleCount);
    if (style < 0) {
        return send_error(req, "400 Bad Request", "style must be 0, 1 or 2");
    }
    Command command;
    command.type = CommandType::Face;
    command.a    = style;
    push_command(std::move(command));
    return send_ok(req);
}

esp_err_t not_found_handler(httpd_req_t* req, httpd_err_code_t /*error*/)
{
    return send_error(req, "404 Not Found", "not found");
}

/* ------------------------------ Stream server ------------------------------ */

/**
 * Streams MJPEG to one viewer until it leaves or the server stops. All locals live here so they are destroyed
 * before the task deletes itself.
 * Returns false if the connection broke.
 */
bool serve_stream(httpd_req_t* req)
{
    auto& s       = state();
    auto& grabber = FrameGrabber::instance();

    // Every frame is one chunk; without Nagle the chunk trailer never waits for a delayed ACK of the viewer
    int sockfd  = httpd_req_to_sockfd(req);
    int nodelay = 1;
    if (sockfd >= 0) {
        setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    }

    static const std::string content_type = utils::mjpegContentType();
    httpd_resp_set_type(req, content_type.c_str());
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");

    std::string packet;  // part header + JPEG, reused between frames
    packet.reserve(32 * 1024);

    uint32_t seq            = 0;
    uint32_t window_frames  = 0;
    int64_t window_start_us = esp_timer_get_time();
    while (!s.stopping.load()) {
        JpegFrame frame;
        if (!grabber.waitJpeg(seq, frame, 500)) {
            // waitJpeg() already waited the full timeout; the extra delay only matters if that ever changes
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        // Only the newest frame is ever sent: a slow viewer skips frames instead of falling behind
        seq = frame.seq;
        packet.assign(utils::mjpegPartHeader(frame.data->size(), frame.timestampUs));
        packet.append(reinterpret_cast<const char*>(frame.data->data()), frame.data->size());
        if (httpd_resp_send_chunk(req, packet.data(), packet.size()) != ESP_OK) {
            return false;
        }

        window_frames++;
        int64_t now = esp_timer_get_time();
        if (now - window_start_us >= 2000000) {
            s.stream_fps_x10.store(static_cast<uint32_t>(window_frames * 10000000LL / (now - window_start_us)));
            window_frames   = 0;
            window_start_us = now;
        }
    }
    return true;
}

void stream_task(void* param)
{
    auto* req     = static_cast<httpd_req_t*>(param);
    auto& s       = state();
    auto& grabber = FrameGrabber::instance();

    grabber.addJpegConsumer();
    mclog::tagInfo(kTag, "stream viewer joined ({} watching)", s.stream_clients.load());

    bool connected = serve_stream(req);
    if (connected) {
        httpd_resp_send_chunk(req, nullptr, 0);
    } else {
        // The viewer is gone: free the socket right away instead of waiting for the session to time out
        httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
    }

    grabber.removeJpegConsumer();
    httpd_req_async_handler_complete(req);
    int remaining = s.stream_clients.fetch_sub(1) - 1;
    if (remaining == 0) {
        s.stream_fps_x10.store(0);
    }
    mclog::tagInfo(kTag, "stream viewer left ({} watching)", remaining);
    vTaskDelete(nullptr);
}

esp_err_t stream_handler(httpd_req_t* req)
{
    if (!authorize(req)) {
        return ESP_OK;
    }
    auto& s = state();
    if (s.stopping.load()) {
        return send_error(req, "503 Service Unavailable", "server stopping");
    }
    if (!FrameGrabber::instance().isRunning()) {
        return send_error(req, "503 Service Unavailable", "camera unavailable");
    }

    int current = s.stream_clients.load();
    do {
        if (current >= kMaxStreamClients) {
            return send_error(req, "503 Service Unavailable", "too many viewers");
        }
    } while (!s.stream_clients.compare_exchange_weak(current, current + 1));

    // Hand the request over to a dedicated task so the server task stays free for other viewers
    httpd_req_t* async_req = nullptr;
    if (httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
        s.stream_clients.fetch_sub(1);
        return ESP_FAIL;
    }
    if (xTaskCreate(stream_task, "mjpeg_stream", kStreamTaskStack, async_req, kStreamTaskPrio, nullptr) != pdPASS) {
        mclog::tagError(kTag, "failed to create stream task");
        httpd_req_async_handler_complete(async_req);
        s.stream_clients.fetch_sub(1);
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool register_uri(httpd_handle_t server, const char* uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t*))
{
    httpd_uri_t config = {};
    config.uri         = uri;
    config.method      = method;
    config.handler     = handler;
    config.user_ctx    = nullptr;
    if (httpd_register_uri_handler(server, &config) != ESP_OK) {
        mclog::tagError(kTag, "failed to register {}", uri);
        return false;
    }
    return true;
}

}  // namespace

WebServer& WebServer::instance()
{
    static WebServer server;
    return server;
}

bool WebServer::start()
{
    auto& s = state();
    std::lock_guard<std::mutex> lifecycle(s.lifecycle);
    if (s.running.load()) {
        return true;
    }
    s.stopping.store(false);

    httpd_config_t control_config    = HTTPD_DEFAULT_CONFIG();
    control_config.server_port       = kControlPort;
    control_config.ctrl_port         = kControlCtrlPort;
    control_config.max_open_sockets  = kControlMaxSockets;
    control_config.max_uri_handlers  = 20;
    control_config.lru_purge_enable  = true;  // browsers keep idle keep-alive connections around
    control_config.stack_size        = kControlStackSize;
    control_config.recv_wait_timeout = 5;
    control_config.send_wait_timeout = 5;
    if (httpd_start(&s.control, &control_config) != ESP_OK) {
        mclog::tagError(kTag, "failed to start control server on port {}", kControlPort);
        s.control = nullptr;
        return false;
    }
    register_uri(s.control, "/", HTTP_GET, index_handler);
    register_uri(s.control, "/index.html", HTTP_GET, index_handler);
    register_uri(s.control, "/favicon.ico", HTTP_GET, favicon_handler);
    register_uri(s.control, "/api/status", HTTP_GET, status_handler);
    register_uri(s.control, "/snapshot.jpg", HTTP_GET, snapshot_handler);
    register_uri(s.control, "/photo.jpg", HTTP_GET, photo_get_handler);
    register_uri(s.control, "/api/photo", HTTP_POST, photo_request_handler);
    register_uri(s.control, "/api/look", HTTP_POST, look_handler);
    register_uri(s.control, "/api/home", HTTP_POST, home_handler);
    register_uri(s.control, "/api/emotion", HTTP_POST, emotion_handler);
    register_uri(s.control, "/api/decorator", HTTP_POST, decorator_handler);
    register_uri(s.control, "/api/rgb", HTTP_POST, rgb_handler);
    register_uri(s.control, "/api/action", HTTP_POST, action_handler);
    register_uri(s.control, "/api/speak", HTTP_POST, speak_handler);
    register_uri(s.control, "/api/sfx", HTTP_POST, sfx_handler);
    register_uri(s.control, "/api/volume", HTTP_POST, volume_handler);
    register_uri(s.control, "/api/face", HTTP_POST, face_handler);
#if STACKPET_FD_BENCH
    register_uri(s.control, "/debug/rgb565", HTTP_GET, debug_rgb_handler);
#endif
#if STACKPET_CAL
    register_uri(s.control, "/debug/faces", HTTP_GET, debug_faces_handler);
    register_uri(s.control, "/debug/spin", HTTP_POST, debug_spin_handler);
#endif
    httpd_register_err_handler(s.control, HTTPD_404_NOT_FOUND, not_found_handler);

    httpd_config_t stream_config    = HTTPD_DEFAULT_CONFIG();
    stream_config.server_port       = kStreamPort;
    stream_config.ctrl_port         = kStreamCtrlPort;
    stream_config.max_open_sockets  = kMaxStreamClients + 1;  // +1 to answer "too many viewers"
    stream_config.max_uri_handlers  = 2;
    stream_config.stack_size        = kStreamServerStack;
    stream_config.send_wait_timeout = kStreamSendTimeoutS;
    if (httpd_start(&s.stream, &stream_config) == ESP_OK) {
        register_uri(s.stream, "/stream", HTTP_GET, stream_handler);
    } else {
        mclog::tagError(kTag, "failed to start stream server on port {}", kStreamPort);
        s.stream = nullptr;
    }

    s.running.store(true);
    mclog::tagInfo(kTag, "started: control :{}, stream :{}", kControlPort, kStreamPort);
    return true;
}

void WebServer::stop()
{
    auto& s = state();
    std::lock_guard<std::mutex> lifecycle(s.lifecycle);
    if (!s.running.load()) {
        return;
    }

    // Let the stream tasks finish first: they use their async request, which must not outlive the server
    s.stopping.store(true);
    uint32_t waited = 0;
    while (s.stream_clients.load() > 0 && waited < kStopTimeoutMs) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20;
    }
    if (s.stream_clients.load() > 0) {
        mclog::tagError(kTag, "{} stream task(s) still running, keep the stream server alive",
                        s.stream_clients.load());
    } else if (s.stream) {
        httpd_stop(s.stream);
        s.stream = nullptr;
    }
    if (s.control) {
        httpd_stop(s.control);
        s.control = nullptr;
    }
    s.running.store(false);
    mclog::tagInfo(kTag, "stopped");
}

bool WebServer::isRunning() const
{
    return state().running.load();
}

void WebServer::setAccessToken(const std::string& token)
{
    std::lock_guard<std::mutex> lock(state().mutex);
    state().token = token;
}

bool WebServer::popCommand(Command& out)
{
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.commands.empty()) {
        return false;
    }
    out = std::move(s.commands.front());
    s.commands.pop_front();
    return true;
}

void WebServer::setStatus(const StatusSnapshot& status)
{
    std::lock_guard<std::mutex> lock(state().mutex);
    state().status = status;
}

void WebServer::publishPhoto(uint32_t id, std::shared_ptr<const std::vector<uint8_t>> jpeg)
{
    std::lock_guard<std::mutex> lock(state().mutex);
    state().photo_id = id;
    state().photo    = std::move(jpeg);
}

int WebServer::streamClientCount() const
{
    return state().stream_clients.load();
}

uint32_t WebServer::lastRequestMs() const
{
    return state().last_request_ms.load();
}

}  // namespace stackpet::web
