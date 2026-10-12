/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "classic_painter.h"
#include <stackchan/avatar/avatar/elements/feature.h>
#include <lvgl.h>
#include <cstdint>

namespace stackpet::face::classic {

/**
 * Base of the "经典加强" eyes and mouth: one transparent, non clickable LVGL object drawn in LV_EVENT_DRAW_MAIN
 * (clicks fall through to the avatar panel), plus the emotion transition: the old shape collapses into a line,
 * the shape is swapped and the new one opens again (~155 ms, like a quick blink). The transition is an lv_anim,
 * so it runs at the LVGL frame rate even when the avatar update loop is slow (xiaozhi updates every ~120 ms while
 * it talks). Nothing is redrawn while the face is still.
 */
class ClassicFeature : public stackchan::avatar::Feature {
public:
    ~ClassicFeature() override;

    void setEmotion(const stackchan::avatar::Emotion& emotion) override;
    stackchan::avatar::Emotion getEmotion() const override;
    void setVisible(bool visible) override;

protected:
    ClassicFeature(lv_obj_t* parent, int width, int height);

    // Position of the object center (= drawing origin), relative to the panel center
    void place(int x, int y);
    void redraw();

    // 1: shape fully shown, 0: collapsed into a line (middle of an emotion transition)
    float morph() const
    {
        return _morph;
    }
    // Emotion currently drawn (switches to the target in the middle of the transition)
    stackchan::avatar::Emotion shown() const
    {
        return _shown;
    }

    virtual void draw(Painter& painter) = 0;

private:
    static void draw_event_cb(lv_event_t* e);
    static void delete_event_cb(lv_event_t* e);
    static void anim_exec_cb(void* var, int32_t value);
    void start_transition(int32_t fromMs);

    // Owned, but usually the avatar panel (owned by the speech bubble element, which is destroyed first) deletes it
    // together with its other children before this feature is destroyed: delete_event_cb then clears it
    lv_obj_t* _obj                     = nullptr;
    stackchan::avatar::Emotion _shown  = stackchan::avatar::Emotion::Neutral;
    stackchan::avatar::Emotion _target = stackchan::avatar::Emotion::Neutral;
    float _morph                       = 1.0f;
    bool _swapped                      = true;
};

class ClassicEye : public ClassicFeature {
public:
    ClassicEye(lv_obj_t* parent, bool isLeftEye);

    void setPosition(const uitk::Vector2i& position) override;
    void setWeight(int weight) override;
    void setSize(int size) override;
    void setEmotion(const stackchan::avatar::Emotion& emotion) override;

protected:
    void draw(Painter& painter) override;

private:
    bool _is_left_eye;
};

class ClassicMouth : public ClassicFeature {
public:
    explicit ClassicMouth(lv_obj_t* parent);

    void setPosition(const uitk::Vector2i& position) override;
    void setWeight(int weight) override;
    void setRotation(int rotation) override;

protected:
    void draw(Painter& painter) override;

private:
    // Tilt of the line shapes in degrees, applied by the painter (no LVGL transform, which would re-render the
    // object through a layer)
    float _tilt = 0.0f;
};

}  // namespace stackpet::face::classic
