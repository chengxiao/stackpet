/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "bigeyes_draw.h"
#include "bigeyes_shapes.h"
#include <stackchan/avatar/avatar/elements/feature.h>
#include <lvgl.h>
#include <cstdint>

namespace stackpet::face::bigeyes {

/**
 * One custom drawn LVGL object. Its draw list is anchored at a point of the panel (e.g. the eye centre) and the
 * object is kept exactly as large as the drawing, so LVGL only invalidates and redraws what really changed.
 * The object never takes clicks: taps fall through to the avatar panel.
 */
class Part {
public:
    Part() = default;
    ~Part();
    Part(const Part&)            = delete;
    Part& operator=(const Part&) = delete;

    void create(lv_obj_t* parent);
    // anchor: panel coordinates of the list origin. An empty list hides the object.
    void update(int32_t anchorX, int32_t anchorY, const DrawList& list);
    void hide();
    lv_obj_t* obj() const
    {
        return _obj;
    }
    // Pivot (object coordinates) of the list origin, for transforms
    int32_t pivotX() const
    {
        return -_bounds.x1;
    }
    int32_t pivotY() const
    {
        return -_bounds.y1;
    }

private:
    static void on_draw(lv_event_t* e);
    static void on_delete(lv_event_t* e);

    lv_obj_t* _obj = nullptr;
    DrawList _list;
    lv_area_t _bounds = {0, 0, 0, 0};
    bool _shown       = false;
};

/**
 * Big glowing eye (see stackpet/face/README.md, style B). All animation runs from its own LVGL timer, so it stays
 * smooth no matter how often the app calls Avatar::update(); the timer pauses itself when nothing moves.
 */
class BigEyesEye : public stackchan::avatar::Feature {
public:
    BigEyesEye(lv_obj_t* parent, bool isLeftEye);
    ~BigEyesEye();

    void setPosition(const uitk::Vector2i& position) override;
    void setWeight(int weight) override;
    void setSize(int size) override;
    void setRotation(int rotation) override;
    void setEmotion(const stackchan::avatar::Emotion& emotion) override;
    stackchan::avatar::Emotion getEmotion() const override
    {
        return _emotion;
    }
    void setVisible(bool visible) override;

private:
    enum class Phase : uint8_t {
        Idle,
        Morph,      // body -> body, every dimension interpolated
        SquashOut,  // the old shape closes into the bar
        SquashIn,   // the new shape pops open from the bar
    };

    static void on_timer(lv_timer_t* timer);

    void refresh();  // evaluate every animation at "now" and redraw what changed
    void kick();     // make sure the timer runs
    void start_transition(stackchan::avatar::Emotion emotion, uint32_t now);
    void apply_rotation();
    void build_eye(DrawList& list, const EyeShape& shape, lv_color_t color, float open, float conv, float sx,
                   float sy) const;
    void build_accessories(uint32_t now, int32_t ax, int32_t ay, bool& ambient);

    bool _is_left = true;
    int _in       = 1;  // +1: the inner side (face centre) is towards +x

    stackchan::avatar::Emotion _emotion       = stackchan::avatar::Emotion::Neutral;  // target
    stackchan::avatar::Emotion _shown_emotion = stackchan::avatar::Emotion::Neutral;  // shape on screen

    // Emotion transition
    Phase _phase       = Phase::Idle;
    uint32_t _phase_t0 = 0;
    uint32_t _phase_ms = 0;
    EyeShape _from;      // Morph start / SquashOut shape
    EyeShape _to;        // target shape
    EyeShape _cur;       // shape on screen (interpolated)
    float _squash      = 1.0f;  // current openness factor of the transition
    float _conv        = 0.0f;  // current convergence onto the bar
    float _squash_from = 1.0f;
    float _conv_from   = 0.0f;
    bool _overshoot    = false;  // Morph with ease-out-back (surprised pop)

    // Colour tween
    lv_color_t _color_from;
    lv_color_t _color_to;
    lv_color_t _color;
    uint32_t _color_t0 = 0;
    uint32_t _color_ms = 0;

    // Blink (weight) tween
    float _open      = 1.0f;
    float _open_from = 1.0f;
    float _open_to   = 1.0f;
    uint32_t _open_t0 = 0;
    uint32_t _open_ms = 0;

    uint32_t _ambient_t0 = 0;  // when the current shape appeared (ambient animations are phased from it)

    Part _eye;
    Part _blush;
    Part _tear;   // right eye only
    Part _glyph;  // right eye only: "?" / "Zzz"
    DrawList _scratch;
    int _applied_rotation = 0;
    int32_t _applied_pivot_x = 0;
    int32_t _applied_pivot_y = 0;
    lv_timer_t* _timer = nullptr;
    bool _paused       = false;
};

/**
 * No mouth at rest; a small mouth in the eye colour shows up while talking (weight > 0) and lingers through the
 * short closed phases of speech. Surprised always shows a small "o".
 */
class BigEyesMouth : public stackchan::avatar::Feature {
public:
    explicit BigEyesMouth(lv_obj_t* parent);
    ~BigEyesMouth();

    void setPosition(const uitk::Vector2i& position) override;
    void setWeight(int weight) override;
    void setRotation(int rotation) override;
    void setEmotion(const stackchan::avatar::Emotion& emotion) override;
    stackchan::avatar::Emotion getEmotion() const override
    {
        return _emotion;
    }
    void setVisible(bool visible) override;

private:
    static void on_timer(lv_timer_t* timer);

    void refresh();
    void kick();

    stackchan::avatar::Emotion _emotion = stackchan::avatar::Emotion::Neutral;

    lv_color_t _color_from;
    lv_color_t _color_to;
    lv_color_t _color;
    uint32_t _color_t0 = 0;
    uint32_t _color_ms = 0;

    float _open       = 0.0f;
    float _open_from  = 0.0f;
    float _open_to    = 0.0f;
    uint32_t _open_t0 = 0;
    uint32_t _open_ms = 0;
    uint32_t _last_talk_ms = 0;  // last time the weight was > 0
    bool _talking          = false;

    Part _part;
    DrawList _scratch;
    lv_timer_t* _timer = nullptr;
    bool _paused       = false;
};

}  // namespace stackpet::face::bigeyes
