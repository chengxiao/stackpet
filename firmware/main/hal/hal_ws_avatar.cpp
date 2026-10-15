/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include <stackchan/stackchan.h>
#include "board/hal_bridge.h"
#include <mooncake.h>
#include <mooncake_log.h>
#include <board.h>
#include <web_socket.h>
#include <esp_log.h>
#include <arpa/inet.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <stackpet/camera/frame_grabber.h>
#include <wifi_station.h>
#include <ArduinoJson.hpp>
#include <settings.h>
#include <atomic>
#include <mutex>
#include <queue>
#include <vector>
#include <esp_heap_caps.h>
#include <display.h>
#include <lvgl_image.h>
#include <wifi_manager.h>
#include "utils/jpeg_to_image/jpeg_decoder.h"
#include "utils/secret_logic/secret_logic.h"

static std::string _tag = "WS-Avatar";

static const std::string _setting_ns              = "stackchan";
static const std::string _setting_device_name_key = "device_name";

// Camera streaming to the app. The frames come from stackpet::FrameGrabber (capture + JPEG in its own task) and
// are sent by a dedicated task, so neither capture nor network stalls the mooncake main loop any more.
// The old implementation captured and encoded synchronously in the main loop every 350 ms (700 ms in video
// mode) at JPEG quality 20.
static constexpr uint8_t _stream_jpeg_quality      = 40;
static constexpr uint8_t _stream_max_fps           = 12;
static constexpr uint32_t _stream_interval_ms      = 1000 / 12;  // ~12 fps
static constexpr uint32_t _video_mode_interval_ms  = 1000 / 8;   // ~8 fps, the device also decodes incoming video
static constexpr uint32_t _sender_stop_timeout_ms  = 1500;
static constexpr size_t _max_ws_payload            = 65535 - 5;  // WebSocket::Send limit minus our packet header

class WebSocketAvatar {
public:
    enum class DataType : uint8_t {
        Opus              = 0x01,
        Jpeg              = 0x02,
        ControlAvatar     = 0x03,
        ControlMotion     = 0x04,
        StartCameraStream = 0x05,
        StopCameraStream  = 0x06,
        TextMessage       = 0x07,
        RequestCall       = 0x09,
        DeclineCall       = 0x0A,
        AcceptCall        = 0x0B,
        EndCall           = 0x0C,
        SetDeviceName     = 0x0D,
        GetDeviceName     = 0x0E,
        HeartbeatPing     = 0x10,
        HeartbeatPong     = 0x11,
        VideoModeOn       = 0x12,
        VideoModeOff      = 0x13,
        DanceSequence     = 0x14,
        StartAudioStream  = 0x18,
        StopAudioStream   = 0x19,
    };

    struct ReceivedMessage {
        bool binary;
        std::vector<uint8_t> data;
    };

    void init()
    {
        _url = fmt::format("{}/stackChan/ws?deviceType=StackChan", secret_logic::get_server_url());

        connect();

        GetHAL().onWsCallResponse.connect([this](bool accepted) {
            if (!isConnected()) {
                return;
            }

            if (accepted) {
                ESP_LOGI(_tag.c_str(), "Sending AcceptCall");
                sendPacket(DataType::AcceptCall, nullptr, 0);
            } else {
                ESP_LOGI(_tag.c_str(), "Sending DeclineCall");
                sendPacket(DataType::DeclineCall, nullptr, 0);
            }
        });

        GetHAL().onWsCallEnd.connect([this](WsSignalSource source) {
            if (!isConnected()) {
                return;
            }

            if (source != WsSignalSource::Local) {
                return;
            }

            ESP_LOGI(_tag.c_str(), "Sending EndCall");
            sendPacket(DataType::EndCall, nullptr, 0);
        });
    }

    ~WebSocketAvatar()
    {
        // Blocks until the camera sender is really gone: it runs on this object, so returning earlier would be a
        // use-after-free if it is still inside Send()
        stop_streaming(portMAX_DELAY);
    }

    void connect()
    {
        auto token = secret_logic::generate_auth_token();

        // 销毁旧实例，确保状态复位. Only the pointer swaps happen under _send_mutex: the camera sender task never
        // sees a half-built or destroyed connection, and it is not blocked during the slow Connect() below.
        std::unique_ptr<WebSocket> old_websocket;
        {
            std::lock_guard<std::mutex> lock(_send_mutex);
            old_websocket = std::move(_websocket);
        }
        old_websocket.reset();

        auto& board  = Board::GetInstance();
        auto network = board.GetNetwork();

        // 创建 WebSocket 实例
        auto websocket = network->CreateWebSocket(1);

        if (!websocket) {
            ESP_LOGE(_tag.c_str(), "Failed to create websocket");
            return;
        }
        WebSocket* ws = websocket.get();

        // 设置认证头
        ws->SetHeader("Authorization", token.c_str());

        // 设置回调. OnConnected() runs inside Connect(), before the new instance is published in _websocket,
        // so it uses its own instance directly.
        ws->OnConnected([this, ws]() {
            ESP_LOGI(_tag.c_str(), "Connected to server!");
            // GetHAL().onWsLog.emit(CommonLogLevel::Info, "Server connected");
            _last_heartbeat_time = GetHAL().millis();
            ws->Send("{\"type\":\"hello\", \"msg\":\"Hello from StackChan!\"}");
        });

        ws->OnDisconnected([this]() {
            ESP_LOGI(_tag.c_str(), "Disconnected!");
            // GetHAL().onWsLog.emit(CommonLogLevel::Error, "Server disconnected");
        });

        ws->OnData([this](const char* data, size_t len, bool binary) {
            std::lock_guard<std::mutex> lock(_mutex);
            _msg_queue.push({binary, std::vector<uint8_t>(data, data + len)});
        });

        // ESP_LOGI(_tag.c_str(), "Connecting to %s...", _url.c_str());
        // GetHAL().onWsLog.emit(CommonLogLevel::Info, "Connecting to server...");
        if (!ws->Connect(_url.c_str())) {
            ESP_LOGE(_tag.c_str(), "Failed to connect");
            GetHAL().onWsLog.emit(CommonLogLevel::Error, "Connect to server Failed");
        }
        _last_reconnect_attempt = GetHAL().millis();

        {
            std::lock_guard<std::mutex> lock(_send_mutex);
            _websocket = std::move(websocket);
        }
    }

    void update()
    {
        if (!_websocket) {
            return;
        }

        if (!_websocket->IsConnected()) {
            if (GetHAL().millis() - _last_reconnect_attempt > 5000) {
                connect();
            }
        } else {
            processMessages();

            // Check heartbeat timeout
            if (GetHAL().millis() - _last_heartbeat_time > 10000) {
                ESP_LOGE(_tag.c_str(), "Heartbeat timeout!");
                GetHAL().onWsLog.emit(CommonLogLevel::Error, "Heartbeat Timeout");
                _last_heartbeat_time = GetHAL().millis();
                return;
            }
        }
    }

    void processMessages()
    {
        std::vector<ReceivedMessage> messages;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            while (!_msg_queue.empty()) {
                messages.push_back(std::move(_msg_queue.front()));
                _msg_queue.pop();
            }
        }

        for (const auto& msg : messages) {
            handleMessage(msg);
        }
    }

    void handleMessage(const ReceivedMessage& msg)
    {
        if (msg.binary) {
            if (msg.data.size() < 1) return;
            DataType type = static_cast<DataType>(msg.data[0]);
            if (type == DataType::Jpeg || type == DataType::Opus) {
                // Video / audio frames arrive many times per second
                ESP_LOGD(_tag.c_str(), "Received binary type: %d, len: %d", (int)type, (int)msg.data.size());
            } else {
                ESP_LOGI(_tag.c_str(), "Received binary type: %d, len: %d", (int)type, (int)msg.data.size());
            }

            switch (type) {
                // Opus handled in OnData Fast Path
                // case DataType::Opus: {
                //     if (msg.data.size() > 5) {
                //         auto packet = std::make_unique<AudioStreamPacket>();
                //         packet->payload.assign(msg.data.begin() + 5, msg.data.end());
                //         _audio_service.PushPacketToDecodeQueue(std::move(packet));
                //     }
                //     break;
                // }
                case DataType::StartCameraStream: {
                    ESP_LOGI(_tag.c_str(), "Start Camera Stream");
                    setStreamingEnabled(true);
                    if (_websocket) {
                        _websocket->Send("camera stream started");
                    }
                    break;
                }
                case DataType::StopCameraStream: {
                    ESP_LOGI(_tag.c_str(), "Stop Camera Stream");
                    setStreamingEnabled(false);
                    if (_websocket) {
                        _websocket->Send("camera stream stopped");
                    }
                    break;
                }
                case DataType::ControlAvatar: {
                    // Protocol: [Type(1)] [Length(4)] [Payload]
                    if (msg.data.size() >= 5) {
                        std::string payload(msg.data.begin() + 5, msg.data.end());
                        // ESP_LOGI(_tag.c_str(), "Control Avatar Payload: %s", payload.c_str());
                        GetHAL().onWsAvatarData.emit(payload);
                    }
                    break;
                }
                case DataType::ControlMotion: {
                    // Protocol: [Type(1)] [Length(4)] [Payload]
                    if (msg.data.size() >= 5) {
                        std::string payload(msg.data.begin() + 5, msg.data.end());
                        // ESP_LOGI(_tag.c_str(), "Control Motion Payload: %s", payload.c_str());
                        GetHAL().onWsMotionData.emit(payload);
                    }
                    break;
                }
                case DataType::RequestCall: {
                    // Protocol: [Type(1)] [Length(4)] [Payload]
                    if (msg.data.size() >= 5) {
                        std::string payload(msg.data.begin() + 5, msg.data.end());
                        ESP_LOGI(_tag.c_str(), "RequestCall Payload: %s", payload.c_str());
                        GetHAL().onWsCallRequest.emit(payload);
                    }
                    break;
                }
                case DataType::EndCall: {
                    ESP_LOGI(_tag.c_str(), "EndCall");
                    GetHAL().onWsCallEnd.emit(WsSignalSource::Remote);
                    break;
                }
                case DataType::SetDeviceName: {
                    // Protocol: [Type(1)] [Length(4)] [Payload]
                    if (msg.data.size() >= 5) {
                        std::string payload(msg.data.begin() + 5, msg.data.end());
                        ESP_LOGI(_tag.c_str(), "SetDeviceName Payload: %s", payload.c_str());

                        Settings settings(_setting_ns, true);
                        settings.SetString(_setting_device_name_key, payload);
                    }
                    break;
                }
                case DataType::GetDeviceName: {
                    ESP_LOGI(_tag.c_str(), "GetDeviceName");

                    Settings settings(_setting_ns, false);
                    auto device_name = settings.GetString(_setting_device_name_key, "StackChan");

                    sendPacket(DataType::GetDeviceName, (const uint8_t*)device_name.c_str(), device_name.size());
                    break;
                }
                case DataType::HeartbeatPing: {
                    ESP_LOGI(_tag.c_str(), "HeartbeatPing");
                    _last_heartbeat_time = GetHAL().millis();
                    sendPacket(DataType::HeartbeatPong, nullptr, 0);
                    break;
                }
                case DataType::TextMessage: {
                    // Protocol: [Type(1)] [Length(4)] [Payload]
                    if (msg.data.size() >= 5) {
                        std::string payload(msg.data.begin() + 5, msg.data.end());
                        ESP_LOGI(_tag.c_str(), "TextMessage Payload: %s", payload.c_str());

                        ArduinoJson::JsonDocument doc;
                        auto error = ArduinoJson::deserializeJson(doc, payload);
                        if (error) {
                            ESP_LOGE(_tag.c_str(), "DeserializeJson failed: %s", error.c_str());
                            return;
                        }

                        WsTextMessage_t text_msg;

                        if (doc["name"].is<std::string>()) {
                            text_msg.name = doc["name"].as<std::string>();
                        }
                        if (doc["content"].is<std::string>()) {
                            text_msg.content = doc["content"].as<std::string>();
                        }

                        GetHAL().onWsTextMessage.emit(text_msg);
                    }
                    break;
                }
                case DataType::VideoModeOn: {
                    ESP_LOGI(_tag.c_str(), "VideoModeOn");
                    GetHAL().onWsVideoModeChange.emit(true);
                    _is_video_mode = true;
                    break;
                }
                case DataType::VideoModeOff: {
                    ESP_LOGI(_tag.c_str(), "VideoModeOff");
                    GetHAL().onWsVideoModeChange.emit(false);
                    _is_video_mode = false;
                    break;
                }
                case DataType::Jpeg: {
                    // Protocol: [Type(1)] [Length(4)] [Payload]
                    if (msg.data.size() >= 5) {
                        ESP_LOGD(_tag.c_str(), "Jpeg Frame Received, size: %d", (int)(msg.data.size() - 5));

                        static int64_t _time_count = 0;
                        static int64_t _interval   = 0;
                        _time_count                = esp_timer_get_time();

                        size_t jpeg_len    = msg.data.size() - 5;
                        uint8_t* jpeg_data = (uint8_t*)heap_caps_malloc(jpeg_len, MALLOC_CAP_8BIT);
                        if (jpeg_data) {
                            memcpy(jpeg_data, msg.data.data() + 5, jpeg_len);

                            auto image = jpeg_dec::decode_to_lvgl(jpeg_data, jpeg_len);
                            if (image) {
                                // ESP_LOGI(_tag.c_str(), "Done");

                                _interval = esp_timer_get_time() - _time_count;
                                mclog::debug("jpeg decode time: {} ms", _interval / 1000);

                                GetHAL().onWsVideoFrame.emit(image);
                            } else {
                                ESP_LOGE(_tag.c_str(), "Failed to decode JPEG");
                            }
                            heap_caps_free(jpeg_data);
                        } else {
                            ESP_LOGE(_tag.c_str(), "Failed to allocate memory for JPEG");
                        }
                    }
                    break;
                }
                case DataType::DanceSequence: {
                    // Protocol: [Type(1)] [Length(4)] [Payload]
                    if (msg.data.size() >= 5) {
                        std::string payload(msg.data.begin() + 5, msg.data.end());
                        // ESP_LOGI(_tag.c_str(), "Dance Payload:\n%s", payload.c_str());
                        ESP_LOGI(_tag.c_str(), "DanceSequence size: %d", (int)payload.size());
                        GetHAL().onWsDanceData.emit(payload);
                    }
                    break;
                }
                case DataType::StartAudioStream: {
                    break;
                }
                case DataType::StopAudioStream: {
                    break;
                }
                default:
                    break;
            }
        } else {
            ESP_LOGI(_tag.c_str(), "Received text: %.*s", (int)msg.data.size(), (char*)msg.data.data());
        }
    }

    bool isConnected()
    {
        return _websocket && _websocket->IsConnected();
    }

    void setStreamingEnabled(bool enabled)
    {
        if (enabled == _is_streaming.load()) {
            return;
        }
        if (enabled) {
            start_streaming();
        } else {
            stop_streaming(pdMS_TO_TICKS(_sender_stop_timeout_ms));
        }
    }

private:
    std::unique_ptr<WebSocket> _websocket;
    std::string _url;
    uint32_t _last_reconnect_attempt = 0;
    uint32_t _last_heartbeat_time    = 0;
    std::atomic<bool> _is_streaming{false};
    std::atomic<bool> _is_video_mode{false};
    TaskHandle_t _sender_task   = nullptr;  // set while a sender task exists (joined via sender_events())
    bool _grabber_started_by_us = false;
    bool _jpeg_consumer_added   = false;
    bool _wifi_performance      = false;
    std::mutex _mutex;
    std::queue<ReceivedMessage> _msg_queue;

    std::mutex _send_mutex;

    void sendPacket(DataType type, const uint8_t* data, size_t len)
    {
        std::lock_guard<std::mutex> lock(_send_mutex);
        if (!_websocket || !_websocket->IsConnected()) {
            return;
        }

        // mclog::info("sending packet type: {}, len: {}", (int)type, (int)len);

        // static int64_t _time_count = 0;
        // static int64_t _interval   = 0;
        // _time_count                = esp_timer_get_time();

        std::vector<uint8_t> packet;
        packet.reserve(1 + 4 + len);

        // [1 byte type]
        packet.push_back(static_cast<uint8_t>(type));

        // [4 bytes length] (Big Endian)
        uint32_t net_len       = htonl((uint32_t)len);
        const uint8_t* len_ptr = (const uint8_t*)&net_len;
        packet.push_back(len_ptr[0]);
        packet.push_back(len_ptr[1]);
        packet.push_back(len_ptr[2]);
        packet.push_back(len_ptr[3]);

        // [payload]
        if (len > 0) {
            packet.insert(packet.end(), data, data + len);
        }

        // _interval = esp_timer_get_time() - _time_count;
        // mclog::info("pack time: {} ms, size: {}", _interval / 1000, packet.size());

        // _time_count = esp_timer_get_time();
        _websocket->Send(packet.data(), packet.size(), true);
        // _interval = esp_timer_get_time() - _time_count;
        // mclog::info("send time: {} ms, size: {}", _interval / 1000, packet.size());
    }

    static constexpr EventBits_t _sender_exited_bit = BIT0;

    // One event group for the whole process: there is a single WebSocketAvatar and it is never deleted while a
    // sender could still signal it, so the group itself is never freed either.
    static EventGroupHandle_t sender_events()
    {
        static EventGroupHandle_t events = xEventGroupCreate();
        return events;
    }

    // Wait for the sender task to exit. Returns true when there is no sender (any more).
    bool join_sender(TickType_t timeout)
    {
        if (_sender_task == nullptr) {
            return true;
        }
        EventBits_t bits = xEventGroupWaitBits(sender_events(), _sender_exited_bit, pdTRUE, pdTRUE, timeout);
        if ((bits & _sender_exited_bit) == 0) {
            return false;
        }
        _sender_task = nullptr;
        return true;
    }

    void start_streaming()
    {
        // Never two senders: a previous one must have exited first
        if (!join_sender(pdMS_TO_TICKS(_sender_stop_timeout_ms))) {
            ESP_LOGE(_tag.c_str(), "Previous camera sender still running, not starting a new one");
            return;
        }

        auto& grabber = stackpet::FrameGrabber::instance();
        if (!grabber.isRunning()) {
            stackpet::GrabberConfig config;
            config.jpegQuality     = _stream_jpeg_quality;
            config.maxFps          = _stream_max_fps;
            config.lumaWidth       = 0;  // streaming only, no vision
            config.lumaHeight      = 0;
            _grabber_started_by_us = grabber.start(config);
            if (!_grabber_started_by_us) {
                ESP_LOGE(_tag.c_str(), "Camera unavailable, cannot stream");
                return;
            }
        }
        if (!_jpeg_consumer_added) {
            grabber.addJpegConsumer();
            _jpeg_consumer_added = true;
        }

        _is_streaming = true;
        xEventGroupClearBits(sender_events(), _sender_exited_bit);
        if (xTaskCreate(&WebSocketAvatar::sender_task_entry, "ws_cam_sender", 4096, this, 4, &_sender_task) != pdPASS) {
            ESP_LOGE(_tag.c_str(), "Failed to create camera sender task");
            _sender_task  = nullptr;
            _is_streaming = false;
            release_camera();
            return;
        }

        // Modem sleep delays TCP ACKs by a DTIM period and throttles the upload
        WifiManager::GetInstance().SetPowerSaveLevel(WifiPowerSaveLevel::PERFORMANCE);
        _wifi_performance = true;
    }

    void stop_streaming(TickType_t joinTimeout)
    {
        _is_streaming = false;
        // The sender wakes up at least every 300 ms (waitJpeg timeout) or after its current Send()
        if (!join_sender(joinTimeout)) {
            ESP_LOGE(_tag.c_str(), "Camera sender task did not stop in time");
        }
        release_camera();
        if (_wifi_performance) {
            // StackPet: not while ESP-NOW listens on this WiFi for the remote (power save would miss it)
            if (!hal_bridge::espnow_keeps_radio_awake()) {
                WifiManager::GetInstance().SetPowerSaveLevel(WifiPowerSaveLevel::BALANCED);
            }
            _wifi_performance = false;
        }
    }

    void release_camera()
    {
        if (_jpeg_consumer_added) {
            stackpet::FrameGrabber::instance().removeJpegConsumer();
            _jpeg_consumer_added = false;
        }
        if (_grabber_started_by_us) {
            stackpet::FrameGrabber::instance().stop();
            _grabber_started_by_us = false;
        }
    }

    static void sender_task_entry(void* param)
    {
        static_cast<WebSocketAvatar*>(param)->sender_loop();
        // Last action: after this bit is set the owner may be destroyed, so `param` must not be touched again
        xEventGroupSetBits(sender_events(), _sender_exited_bit);
        vTaskDelete(nullptr);
    }

    // All locals live here, so they are destroyed before the task deletes itself
    void sender_loop()
    {
        auto& grabber          = stackpet::FrameGrabber::instance();
        uint32_t seq           = 0;
        uint32_t last_send_ms  = 0;
        uint32_t window_frames = 0;
        uint32_t window_start  = GetHAL().millis();

        while (_is_streaming.load()) {
            stackpet::JpegFrame frame;
            if (!grabber.waitJpeg(seq, frame, 300)) {
                // waitJpeg() already waited the full timeout, even with a stopped grabber; never spin anyway
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            seq = frame.seq;

            // Pace the upload; after waiting, take whatever frame is newest by then
            uint32_t interval = _is_video_mode.load() ? _video_mode_interval_ms : _stream_interval_ms;
            uint32_t elapsed  = GetHAL().millis() - last_send_ms;
            if (elapsed < interval) {
                vTaskDelay(pdMS_TO_TICKS(interval - elapsed));
                stackpet::JpegFrame newer;
                if (grabber.waitJpeg(seq, newer, 0)) {
                    frame = std::move(newer);
                    seq   = frame.seq;
                }
            }
            if (!_is_streaming.load()) {
                break;
            }
            if (frame.data->size() > _max_ws_payload) {
                ESP_LOGW(_tag.c_str(), "JPEG frame too large for one websocket frame: %d", (int)frame.data->size());
                continue;
            }

            sendPacket(DataType::Jpeg, frame.data->data(), frame.data->size());
            last_send_ms = GetHAL().millis();

            window_frames++;
            if (last_send_ms - window_start >= 10000) {
                mclog::tagInfo(_tag, "camera stream: {:.1f} fps, {} bytes/frame",
                               window_frames * 1000.0f / (last_send_ms - window_start), frame.data->size());
                window_frames = 0;
                window_start  = last_send_ms;
            }
        }
    }
};

class WebsocketAvatarWorker : public mooncake::BasicAbility {
public:
    WebsocketAvatarWorker()
    {
        _service = std::make_unique<WebSocketAvatar>();
        _service->init();
    }

    void onCreate() override
    {
    }

    void onRunning() override
    {
        if (GetHAL().millis() - _last_tick < 20) {
            return;
        }
        _last_tick = GetHAL().millis();
        _service->update();
    }

    void onDestroy() override
    {
        _service.reset();
    }

private:
    std::unique_ptr<WebSocketAvatar> _service;
    uint32_t _last_tick = 0;
};

void Hal::startWebSocketAvatarService(std::function<void(std::string_view)> onStartLog)
{
    mclog::tagInfo(_tag, "start websocket avatar service");

    startNetwork(onStartLog);

    onStartLog("Connecting to\nserver...");
    mooncake::GetMooncake().extensionManager()->createAbility(std::make_unique<WebsocketAvatarWorker>());
}
