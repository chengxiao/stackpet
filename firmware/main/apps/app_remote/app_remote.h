/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <mooncake.h>
#include <smooth_lvgl.hpp>
#include <uitk/short_namespace.hpp>
#include <stackchan/avatar/avatar/elements/emotion.h>
#include <stackpet/web/web_server.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

/**
 * @brief LAN remote: a web page (served by the robot itself) shows the live camera and drives the head,
 * the face, the LEDs and the speaker. Everything runs on the local network, no cloud involved; the page only
 * works with the per-session access token carried by the QR code on the screen.
 */
class AppRemote : public mooncake::AppAbility {
public:
    AppRemote();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    struct MotionStep {
        int yaw;
        int pitch;
        int speed;
        uint32_t durationMs;
    };

    enum class Stage { Connecting, Ready };
    enum class SpinState { Idle, Waiting, Rotating, Returning };
    enum class PhotoState { Idle, Countdown, Capturing };

    /* ------------------------------ Wi-Fi bootstrap ----------------------------- */
    static void network_task(void* param);
    void post_network_message(std::string message);
    void update_connecting(uint32_t now);
    void enter_ready(uint32_t now);
    void create_loading_ui();
    void destroy_loading_ui();
    void rebuild_system_widgets();

    /* --------------------------------- Commands -------------------------------- */
    void handle_command(const stackpet::web::Command& command, uint32_t now);
    void create_avatar(stackchan::avatar::Emotion emotion);
    void apply_face_style(int index);
    void handle_action(const std::string& action, uint32_t now);
    void add_decorator(const std::string& name);
    void show_speech(const std::string& text, uint32_t durationMs);

    /* ------------------------------ Motion programs ----------------------------- */
    void start_sequence(std::vector<MotionStep> steps, uint32_t now);
    void update_sequence(uint32_t now);
    void stop_sequence();
    void start_spin(uint32_t now);
    void update_spin(uint32_t now);
#if STACKPET_CAL
    // Test builds only: wheel mode rotation with the raw yaw readings logged (rotation calibration)
    void update_debug_spin(uint32_t now);
    int _debug_spin_velocity       = 0;
    uint32_t _debug_spin_end_ms    = 0;
    std::vector<std::pair<uint32_t, int>> _debug_spin_samples;
#endif
    void update_dance_leds(uint32_t now);

    /* ---------------------------------- Photo ---------------------------------- */
    void start_photo(uint32_t id, uint32_t now);
    void update_photo(uint32_t now);
    void finish_photo();

    /* ------------------------------- Status / UI ------------------------------- */
    void update_status(uint32_t now);
    void update_first_visitor(uint32_t now);
    void update_live_badge(uint32_t now);
    void refresh_live_widgets();
    void create_card(bool serverOk, bool cameraOk);
    void create_live_badge();
    void set_card_visible(bool visible);
    void destroy_widgets();

    Stage _stage = Stage::Connecting;

    // Wi-Fi bootstrap: GetHAL().startNetwork() blocks until connected, so it runs in its own task
    std::mutex _net_mutex;
    std::string _net_message;
    bool _net_message_dirty = false;
    std::atomic<bool> _net_connected{false};
    bool _net_task_started  = false;
    bool _net_timeout_shown = false;
    std::unique_ptr<uitk::lvgl_cpp::Container> _loading_panel;
    std::unique_ptr<uitk::lvgl_cpp::Label> _loading_title;
    std::unique_ptr<uitk::lvgl_cpp::Label> _loading_msg;
    std::unique_ptr<uitk::lvgl_cpp::Label> _loading_hint;

    // QR card
    std::unique_ptr<uitk::lvgl_cpp::Container> _card;
    std::unique_ptr<uitk::lvgl_cpp::Qrcode> _qrcode;
    std::unique_ptr<uitk::lvgl_cpp::Label> _card_title;
    std::unique_ptr<uitk::lvgl_cpp::Label> _card_hint;
    std::unique_ptr<uitk::lvgl_cpp::Label> _card_warning;
    std::unique_ptr<uitk::lvgl_cpp::Container> _card_live_row;  // "● 正在直播 · N人" inside the card
    std::unique_ptr<uitk::lvgl_cpp::Container> _card_live_dot;
    std::unique_ptr<uitk::lvgl_cpp::Label> _card_live_label;
    std::unique_ptr<uitk::lvgl_cpp::Label> _card_url;
    bool _card_visible = false;
    std::atomic<bool> _screen_clicked{false};

    // "● 直播中" badge, shown while somebody watches the camera and the QR card is hidden (it would cover the
    // QR code otherwise; the card then shows its own live row)
    std::unique_ptr<uitk::lvgl_cpp::Container> _live_badge;
    std::unique_ptr<uitk::lvgl_cpp::Container> _live_dot;
    std::unique_ptr<uitk::lvgl_cpp::Label> _live_label;
    int _live_count           = -1;
    uint32_t _live_blink_ms   = 0;
    uint32_t _live_checked_ms = 0;

    std::string _ip;
    std::string _url;     // plain address, for the logs
    std::string _qr_url;  // with the access token: in the QR code and printed on the card (typed by hand)
    uint32_t _opened_ms      = 0;
    uint32_t _last_status_ms = 0;
    uint32_t _last_look_ms   = 0;
    bool _visitor_greeted    = false;

    int _target_yaw         = 0;
    int _target_pitch       = 0;
    uint8_t _rgb[3]         = {0, 0, 0};
    int _speech_modifier_id = -1;

    std::vector<MotionStep> _steps;
    size_t _step_index        = 0;
    uint32_t _step_started_ms = 0;
    bool _dancing             = false;
    int _emotion_before_dance = 0;
    uint32_t _dance_led_ms    = 0;

    SpinState _spin_state   = SpinState::Idle;
    uint32_t _spin_state_ms = 0;
    uint32_t _spin_poll_ms  = 0;
    uint32_t _spin_led_ms   = 0;
    int _spin_velocity      = 0;

    PhotoState _photo_state    = PhotoState::Idle;
    uint32_t _photo_id         = 0;
    uint32_t _photo_started_ms = 0;
    uint32_t _photo_capture_ms = 0;
    uint32_t _photo_seq_mark   = 0;
    int _photo_count_shown     = 0;
    bool _photo_light_on       = false;
    uint32_t _flash_until_ms   = 0;
    std::unique_ptr<uitk::lvgl_cpp::Container> _photo_panel;
    std::unique_ptr<uitk::lvgl_cpp::Label> _photo_label;
    std::unique_ptr<uitk::lvgl_cpp::Container> _flash_panel;
};
