/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <lvgl.h>
#include <stackpet/camera/frame_grabber.h>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace stackpet::radar {

enum class BlipKind : uint8_t { Contact, Locked, Friend, Behind };  // Behind: out of the head's reach (gray)

/**
 * @brief The radar scope (LVGL, call everything with the LVGL lock held).
 *
 * Green rings and ticks, a sweep line that points where the head points, an afterglow sector behind it whose
 * length follows the sweep speed (it shrinks to nothing at the turnarounds instead of flipping sides), fading
 * blips for the targets seen. Only small wedges are invalidated per frame: LVGL renders in software.
 */
class RadarScreen {
public:
    ~RadarScreen();

    void create(lv_obj_t* parent, const lv_font_t* textFont);
    void destroy();
    void setVisible(bool visible);
    bool visible() const;

    // Head yaw (0.1 degree, + = robot's left) and its rate (0.1 degree per second, signed)
    void setSweep(float yaw, float velocity, float maxVelocity);
    void setStatus(std::string_view text, uint32_t color);
    void setBearing(int yaw);
    void setInfo(std::string_view text);
    // distance01: 0 = centre .. 1 = outer ring
    void addBlip(int bearing, float distance01, BlipKind kind, uint32_t nowMs);
    // The two dashed lines: the sweep limits, or where the spin scan can still lock on (+-yaw, 0.1 degree)
    void setLimitYaw(int yaw);
    void ping(uint32_t nowMs);
    void update(uint32_t nowMs);

private:
    void place_limits();

    struct Blip {
        lv_obj_t* obj     = nullptr;
        uint32_t bornMs   = 0;
        uint32_t lifeMs   = 0;
        bool active       = false;
        lv_opa_t shownOpa = 0;
    };

    static constexpr int kBlipCount = 8;
    static constexpr int kTickCount = 12;

    lv_obj_t* make_line(lv_point_precise_t* points, uint32_t count, uint32_t color, int width, lv_opa_t opa);
    lv_obj_t* make_ring(int radius, uint32_t color, int width, lv_opa_t opa);
    lv_obj_t* make_glow(lv_opa_t opa);
    void place_line(lv_obj_t* line, lv_point_precise_t* points, float angleDeg, float r0, float r1);
    void set_glow(lv_obj_t* arc, float fromDeg, float toDeg, bool& shown);

    lv_obj_t* _root         = nullptr;
    lv_obj_t* _glow_far     = nullptr;
    lv_obj_t* _glow_near    = nullptr;
    lv_obj_t* _beam         = nullptr;
    lv_obj_t* _hub          = nullptr;
    lv_obj_t* _status       = nullptr;
    lv_obj_t* _bearing      = nullptr;
    lv_obj_t* _info         = nullptr;
    std::array<lv_obj_t*, kTickCount> _ticks{};
    std::array<std::array<lv_point_precise_t, 2>, kTickCount> _tick_points{};
    std::array<lv_point_precise_t, 2> _beam_points{};
    std::array<std::array<lv_point_precise_t, 2>, 2> _limit_points{};
    std::array<lv_obj_t*, 2> _limit_lines{};
    int _limit_yaw = 1250;
    std::array<std::array<lv_point_precise_t, 2>, 2> _cross_points{};
    std::array<Blip, kBlipCount> _blips{};
    int _next_blip          = 0;
    float _shown_angle      = 1e9f;
    int _shown_trail        = -1;
    bool _far_shown         = false;
    bool _near_shown        = false;
    int _shown_bearing      = 1 << 30;
    uint32_t _ping_until    = 0;
    bool _ping_shown        = false;
    uint32_t _last_fade_ms  = 0;
    std::string _status_text;
    uint32_t _status_color  = 0;
    std::string _info_text;
};

/**
 * @brief The lock-on camera view: the analyzed camera frame full screen, a red box with corner brackets around
 * the target that zooms in from the screen edges ("focusing"), 「锁定目标」 and a flashing red frame.
 */
class LockView {
public:
    ~LockView();

    void create(lv_obj_t* parent, const lv_font_t* titleFont, const lv_font_t* textFont);
    void destroy();
    void show(uint32_t nowMs);
    void hide();
    bool visible() const;

    // Show this frame (kept alive while displayed). Frames smaller than the screen are scaled up.
    void setFrame(const RgbFrame& frame);
    // Target box in normalized image coordinates (centre -1..1, size as a fraction of the image)
    void setTarget(float cx, float cy, float w, float h, float distanceM);
    void update(uint32_t nowMs);

private:
    struct Rect {
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    };

    void apply_rect(const Rect& rect);

    lv_obj_t* _root   = nullptr;
    lv_obj_t* _image  = nullptr;
    lv_obj_t* _border = nullptr;
    lv_obj_t* _box    = nullptr;
    lv_obj_t* _title  = nullptr;
    lv_obj_t* _range  = nullptr;
    std::array<lv_obj_t*, 4> _corners{};
    std::array<std::array<lv_point_precise_t, 3>, 4> _corner_points{};
    std::array<lv_obj_t*, 2> _cross{};
    std::array<std::array<lv_point_precise_t, 2>, 2> _cross_points{};
    lv_image_dsc_t _dsc{};
    RgbFrame _frame;
    bool _has_target    = false;
    Rect _target;
    Rect _shown;
    uint32_t _shown_ms  = 0;
    bool _border_on     = false;
    bool _title_on      = true;
    std::string _range_text;
};

}  // namespace stackpet::radar
