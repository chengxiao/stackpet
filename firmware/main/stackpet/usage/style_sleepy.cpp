/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
// StackPet screensaver style 睡觉: StackChan asleep under the clock. Closed eyes, a little "o" of a mouth, the face
// rising and falling as it breathes, Z's drifting up. Claude waiting for the user: eyes open and a "!"; Claude done: a
// smile. The windows below as two short bars
#include "clock_style.h"
#include <stackpet/ui/ui.h>
#include <cmath>
#include <cstdio>

namespace stackpet::usage {

namespace {

using namespace widgets;

constexpr int kFaceY      = 98;  // the face's part, it breathes up and down from here
constexpr int kFaceH      = 92;
constexpr int kEyeD       = 56;
constexpr int kEyeW       = 6;
constexpr int kEyeX[2]    = {112, 208};  // eye centers in the face's part
constexpr int kEyeCy      = 30;
constexpr int kOpenD      = 26;
constexpr int kBangX      = 256;
constexpr int kBangDy     = -16;  // from the face's top
constexpr int kColW       = 128;
constexpr int kColX[2]    = {24, kScreenW - 24 - kColW};
constexpr int kNameY      = 192;
constexpr int kBarY       = 222;
constexpr int kBarH       = 4;
constexpr uint32_t kBreathMs = 4200;
constexpr uint32_t kZzzMs    = 3000;
constexpr uint32_t kThrob    = 500;
constexpr uint32_t kCheek    = 0xF29BB5;

lv_obj_t* eye_arc(lv_obj_t* parent, int cx, uint32_t color)
{
    lv_obj_t* arc = lv_arc_create(parent);
    lv_obj_remove_style_all(arc);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(arc, kEyeD, kEyeD);
    lv_obj_set_pos(arc, cx - kEyeD / 2, kEyeCy - kEyeD / 2);
    lv_arc_set_bg_angles(arc, 0, 0);
    lv_obj_set_style_arc_width(arc, kEyeW, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, rgb(color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    return arc;
}

class SleepyView : public StyleView {
public:
    explicit SleepyView(lv_obj_t* root) : _p(paletteFor(Style::Sleepy))
    {
        const lv_font_t* cn = ui::fontCn20();
        _clock              = label(root, &font_usage_clock_52, _p.text);
        lv_obj_align(_clock, LV_ALIGN_TOP_MID, 0, 12);
        _info = label(root, cn, _p.muted);
        lv_obj_align(_info, LV_ALIGN_TOP_MID, 0, 58);

        _face = plain(root);
        lv_obj_set_size(_face, kScreenW, kFaceH);
        lv_obj_set_pos(_face, 0, kFaceY);
        for (int i = 0; i < 2; ++i) {
            _cheeks[i] = plain(_face);
            lv_obj_set_size(_cheeks[i], 26, 12);
            lv_obj_set_pos(_cheeks[i], i == 0 ? 70 : 224, 52);
            lv_obj_set_style_radius(_cheeks[i], LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(_cheeks[i], 60, 0);
            lv_obj_set_style_bg_color(_cheeks[i], rgb(kCheek), 0);
            _eyes[i] = eye_arc(_face, kEyeX[i], _p.text);
            _open[i] = dot(_face, kOpenD, _p.text);
            lv_obj_set_pos(_open[i], kEyeX[i] - kOpenD / 2, kEyeCy + 4 - kOpenD / 2);
        }
        _mouth = plain(_face);  // a ring: "o"
        lv_obj_set_size(_mouth, 14, 11);
        lv_obj_set_pos(_mouth, 153, 60);
        lv_obj_set_style_radius(_mouth, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(_mouth, 3, 0);
        lv_obj_set_style_border_color(_mouth, rgb(_p.text), 0);
        lv_obj_set_style_border_opa(_mouth, LV_OPA_COVER, 0);
        _smile = eye_arc(_face, 160, _p.text);  // done: a smile under the closed eyes
        lv_obj_set_size(_smile, 34, 34);
        lv_obj_set_pos(_smile, 160 - 17, 44);
        lv_obj_set_style_arc_width(_smile, 4, LV_PART_INDICATOR);
        lv_arc_set_angles(_smile, 30, 150);
        // "!": a stroke and a dot, drawn (the built-in fonts are too thin for it); above the face's part, moved with it
        _bang = plain(root);
        lv_obj_set_size(_bang, 8, 34);
        lv_obj_set_pos(_bang, kBangX, kFaceY + kBangDy);
        lv_obj_t* stroke = plain(_bang);
        lv_obj_set_size(stroke, 8, 22);
        lv_obj_set_style_radius(stroke, 4, 0);
        lv_obj_set_style_bg_opa(stroke, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(stroke, rgb(_p.wait), 0);
        lv_obj_t* point = dot(_bang, 8, _p.wait);
        lv_obj_set_pos(point, 0, 26);
        for (int i = 0; i < 3; ++i) {
            _z[i] = label(root, i == 2 ? &lv_font_montserrat_24 : &lv_font_montserrat_20, _p.muted);
            lv_label_set_text(_z[i], "Z");
        }

        for (int i = 0; i < 2; ++i) {
            Col& c   = _cols[i];
            c.name   = label(root, cn, _p.muted);
            lv_label_set_text(c.name, i == 0 ? "5 小时" : "本周");
            lv_obj_set_pos(c.name, kColX[i], kNameY);
            c.pct = label(root, &font_usage_num_26, _p.normal);
            lv_obj_set_width(c.pct, kColW);
            lv_obj_set_style_text_align(c.pct, LV_TEXT_ALIGN_RIGHT, 0);
            const int dy =
                (cn->line_height - cn->base_line) - (font_usage_num_26.line_height - font_usage_num_26.base_line);
            lv_obj_set_pos(c.pct, kColX[i], kNameY + dy);
            c.track = bar(root, kColW, kBarH, _p.track, &c.fill);
            lv_obj_set_pos(c.track, kColX[i], kBarY);
        }
        _empty = label(root, cn, _p.muted);
        lv_obj_align(_empty, LV_ALIGN_TOP_MID, 0, kNameY + 6);
    }

    int floatX() const override
    {
        return 10;
    }
    int floatY() const override
    {
        return 5;
    }

    void show(const ClockModel& m, bool night) override
    {
        const std::string note = feedNote(m.status, m.haveSnapshot, m.ageMs);
        const bool fresh       = m.haveSnapshot && note.empty();
        setText(_clock, _shown_clock, clockText(m.local));
        const AgentState state = fresh ? m.snapshot.state : AgentState::Unknown;
        // Under the clock: the date and what Claude is doing, or why the usage is old
        std::string info = dateText(m.local);
        const std::string what = stateText(state, fresh ? m.snapshot.waiting : 0);
        if (m.haveSnapshot && !note.empty()) {
            info = note;
        } else if (!what.empty()) {
            info += info.empty() ? what : " · " + what;
        }
        setText(_info, _shown_info, info);
        setTextColor(_info, _shown_info_color, state == AgentState::Wait ? _p.wait : _p.muted);

        // The face: asleep, eyes open while Claude waits, smiling when it is done
        const int mood = state == AgentState::Wait ? 1 : (state == AgentState::Done ? 2 : 0);
        if (mood != _shown_mood) {
            for (int i = 0; i < 2; ++i) {
                setHidden(_eyes[i], mood == 1);
                setHidden(_open[i], mood != 1);
                lv_arc_set_angles(_eyes[i], mood == 2 ? 205 : 25, mood == 2 ? 335 : 155);  // happy arcs / asleep
            }
            setHidden(_mouth, mood == 2);
            setHidden(_smile, mood != 2);
            setHidden(_bang, mood != 1);
            _shown_mood = mood;
            _shown_bang = -1;
        }
        const int bang = mood == 1 && !night ? 1 : 0;
        if (bang != _shown_bang) {
            if (bang) {
                pulse(_bang, LV_OPA_COVER, 110, kThrob);  // never faint: it is the point
            } else {
                steady(_bang, LV_OPA_COVER);
            }
            _shown_bang = bang;
        }
        // Breathing: a few pixels up and down, slower at night
        const uint32_t breath = night ? kBreathMs * 3 / 2 : kBreathMs;
        const int lift        = static_cast<int>(
            std::lround(3.0 * std::sin(6.283185307179586 * static_cast<double>(m.ms % breath) / breath)));
        if (lift != _shown_lift) {
            lv_obj_set_y(_face, kFaceY + lift);
            lv_obj_set_y(_bang, kFaceY + kBangDy + lift);
            _shown_lift = lift;
        }
        // Z's drifting up from beside the head while asleep, one after the other
        for (int i = 0; i < 3; ++i) {
            const double phase = static_cast<double>((m.ms + i * kZzzMs / 3) % kZzzMs) / kZzzMs;
            // Beside the right eye, up and away to the right (clear of the line under the clock)
            const int x        = 252 + static_cast<int>(phase * 30);
            const int y        = kFaceY + 34 - static_cast<int>(phase * 40);
            const int opa      = mood == 0 ? static_cast<int>(std::sin(phase * 3.14159265358979) * 190) : 0;
            Z& z               = _zs[i];
            if (x != z.x || y != z.y) {
                lv_obj_set_pos(_z[i], x, y);
                z.x = x;
                z.y = y;
            }
            if (opa != z.opa) {
                lv_obj_set_style_opa(_z[i], static_cast<lv_opa_t>(opa), 0);
                z.opa = opa;
            }
        }

        // The two windows; no usage at all: why, in their place
        for (Col& c : _cols) {
            for (lv_obj_t* o : {c.name, c.pct, c.track}) {
                setHidden(o, !m.haveSnapshot);
            }
        }
        setHidden(_empty, m.haveSnapshot);
        if (m.haveSnapshot) {
            show_col(_cols[0], pctNow(m.snapshot.session, m.now), fresh);
            show_col(_cols[1], pctNow(m.snapshot.weekly, m.now), fresh);
        } else {
            setText(_empty, _shown_empty, note);
        }
    }

private:
    struct Col {
        lv_obj_t* name  = nullptr;
        lv_obj_t* pct   = nullptr;
        lv_obj_t* track = nullptr;
        lv_obj_t* fill  = nullptr;
        std::string shown_pct;
        uint32_t shown_color      = 0;
        uint32_t shown_fill_color = 0;
        int shown_w               = -1;
    };
    struct Z {
        int x   = -1000;
        int y   = -1000;
        int opa = -1;
    };

    void show_col(Col& c, int pct, bool fresh)
    {
        char buf[16] = "--%";
        if (pct >= 0) {
            std::snprintf(buf, sizeof(buf), "%d%%", pct);
        }
        setText(c.pct, c.shown_pct, buf);
        const uint32_t color = fresh ? levelColor(_p, levelFor(pct)) : _p.dim;
        setTextColor(c.pct, c.shown_color, color);
        setBgColor(c.fill, c.shown_fill_color, color);
        setBarWidth(c.fill, c.shown_w, barWidth(pct, kColW, kBarH));
    }

    const Palette& _p;
    lv_obj_t* _clock = nullptr;
    lv_obj_t* _info  = nullptr;
    lv_obj_t* _face  = nullptr;
    lv_obj_t* _eyes[2]{};
    lv_obj_t* _open[2]{};
    lv_obj_t* _cheeks[2]{};
    lv_obj_t* _mouth = nullptr;
    lv_obj_t* _smile = nullptr;
    lv_obj_t* _bang  = nullptr;
    lv_obj_t* _z[3]{};
    Z _zs[3];
    Col _cols[2];
    lv_obj_t* _empty = nullptr;
    std::string _shown_clock;
    std::string _shown_info;
    std::string _shown_empty;
    uint32_t _shown_info_color = 0;
    int _shown_mood            = -1;
    int _shown_bang            = -1;
    int _shown_lift            = 1000;
};

}  // namespace

std::unique_ptr<StyleView> makeSleepyView(lv_obj_t* root)
{
    return std::make_unique<SleepyView>(root);
}

}  // namespace stackpet::usage
