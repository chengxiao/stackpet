/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
// StackPet screensaver style 暖黑: Claude's warm black. A spark top left (it turns while Claude works, goes amber and
// throbs while Claude waits), the date, Claude's state; the big cream clock with a thin line under it that fills
// each minute (in the color of the window closest to its limit); the two windows side by side
#include "clock_style.h"
#include <stackpet/ui/ui.h>
#include <cmath>
#include <cstdio>

namespace stackpet::usage {

namespace {

using namespace widgets;

constexpr int kM          = 18;
constexpr int kColW       = 136;
constexpr int kColX[2]    = {kM, kScreenW - kM - kColW};
constexpr int kNameY      = 148;
constexpr int kBarY       = 180;
constexpr int kBarH       = 4;
constexpr int kResetY     = 190;
constexpr int kSweepW     = 184;
constexpr int kSweepY     = 128;
constexpr int kSpark      = 20;
constexpr uint32_t kThrob = 700;

class ClaudeView : public StyleView {
public:
    explicit ClaudeView(lv_obj_t* root) : _p(paletteFor(Style::Claude))
    {
        const lv_font_t* cn = ui::fontCn20();
        _spark              = plain(root);
        lv_obj_set_size(_spark, kSpark, kSpark);
        lv_obj_set_pos(_spark, kM, 13);
        for (Ray& ray : _rays) {
            ray.line = lv_line_create(_spark);
            lv_obj_set_style_line_width(ray.line, 3, 0);
            lv_obj_set_style_line_rounded(ray.line, true, 0);
        }
        spark(0, _p.idle);
        _date = label(root, cn, _p.muted);
        lv_obj_set_pos(_date, kM + kSpark + 10, 10);
        _state = label(root, cn, _p.muted);
        lv_obj_align(_state, LV_ALIGN_TOP_RIGHT, -kM, 10);
        _clock = label(root, &font_usage_clock_96, _p.text);
        lv_obj_align(_clock, LV_ALIGN_TOP_MID, 0, 44);
        _sweep_track = bar(root, kSweepW, 2, _p.track, &_sweep);
        lv_obj_set_pos(_sweep_track, (kScreenW - kSweepW) / 2, kSweepY);
        _note = label(root, cn, _p.muted);
        lv_obj_align(_note, LV_ALIGN_TOP_MID, 0, kSweepY - 8);
        setHidden(_note, true);
        for (int i = 0; i < 2; ++i) {
            build_col(_cols[i], root, i == 0 ? "5 小时" : "本周", kColX[i]);
        }
        _empty = label(root, cn, _p.muted);
        lv_obj_align(_empty, LV_ALIGN_TOP_MID, 0, 168);
        setHidden(_empty, true);
    }

    int floatX() const override
    {
        return 14;
    }
    int floatY() const override
    {
        return 8;
    }

    void show(const ClockModel& m, bool night) override
    {
        const std::string note = feedNote(m.status, m.haveSnapshot, m.ageMs);
        const bool fresh       = m.haveSnapshot && note.empty();
        setText(_date, _shown_date, dateText(m.local));
        setText(_clock, _shown_clock, clockText(m.local));

        // Claude's state, and the spark in its color
        const AgentState state = fresh ? m.snapshot.state : AgentState::Unknown;
        const uint32_t color   = stateColor(_p, state);
        setText(_state, _shown_state, fresh ? stateText(state, m.snapshot.waiting) : "");
        setTextColor(_state, _shown_state_color, state == AgentState::Idle ? _p.muted : color);
        // Turning while Claude works (slower at night), a quarter second a step
        const int angle = state == AgentState::Work ? static_cast<int>(m.ms / (night ? 120 : 40) % 360) : 0;
        if (angle != _shown_angle || color != _shown_spark_color) {
            spark(angle, color);
            _shown_angle       = angle;
            _shown_spark_color = color;
        }
        const int throb = state == AgentState::Wait && !night ? 1 : 0;
        if (throb != _shown_throb) {
            if (throb) {
                pulse(_spark, LV_OPA_COVER, 60, kThrob);
                pulse(_state, LV_OPA_COVER, 90, kThrob);
            } else {
                steady(_spark, LV_OPA_COVER);
                steady(_state, LV_OPA_COVER);
            }
            _shown_throb = throb;
        }

        // The minute filling under the clock, in the color of the window closest to its limit; old usage: why, there
        const Level worst = fresh ? worstLevel(m.snapshot, m.now) : Level::Unknown;
        setBgColor(_sweep, _shown_sweep_color, levelColor(_p, worst == Level::Unknown ? Level::Normal : worst));
        setBarWidth(_sweep, _shown_sweep_w, clockValid(m.local) ? kSweepW * m.local.tm_sec / 59 : 0);
        const bool note_up = m.haveSnapshot && !note.empty();
        setHidden(_sweep_track, note_up);
        setHidden(_note, !note_up);
        if (note_up) {
            setText(_note, _shown_note, note);
        }

        // The two windows; no usage at all: why, in their place
        for (Col& c : _cols) {
            for (lv_obj_t* o : {c.name, c.pct, c.track, c.reset}) {
                setHidden(o, !m.haveSnapshot);
            }
        }
        setHidden(_empty, m.haveSnapshot);
        if (m.haveSnapshot) {
            show_col(_cols[0], m.snapshot.session, m.now, fresh);
            show_col(_cols[1], m.snapshot.weekly, m.now, fresh);
        } else {
            setText(_empty, _shown_empty, note);
        }
    }

private:
    struct Ray {
        lv_obj_t* line = nullptr;
        lv_point_precise_t pts[2]{};
    };
    struct Col {
        lv_obj_t* name  = nullptr;
        lv_obj_t* pct   = nullptr;
        lv_obj_t* track = nullptr;
        lv_obj_t* fill  = nullptr;
        lv_obj_t* reset = nullptr;
        std::string shown_pct;
        std::string shown_reset;
        uint32_t shown_color      = 0;
        uint32_t shown_fill_color = 0;
        int shown_w               = -1;
    };

    void build_col(Col& c, lv_obj_t* root, const char* name, int x)
    {
        const lv_font_t* cn = ui::fontCn20();
        c.name              = label(root, cn, _p.muted);
        lv_label_set_text(c.name, name);
        lv_obj_set_pos(c.name, x, kNameY);
        // The digits on the CN text's baseline, right-aligned in the column
        c.pct = label(root, &font_usage_num_26, _p.normal);
        lv_obj_set_width(c.pct, kColW);
        lv_obj_set_style_text_align(c.pct, LV_TEXT_ALIGN_RIGHT, 0);
        const int dy =
            (cn->line_height - cn->base_line) - (font_usage_num_26.line_height - font_usage_num_26.base_line);
        lv_obj_set_pos(c.pct, x, kNameY + dy);
        c.track = bar(root, kColW, kBarH, _p.track, &c.fill);
        lv_obj_set_pos(c.track, x, kBarY);
        c.reset = label(root, cn, _p.muted);
        lv_obj_set_pos(c.reset, x, kResetY);
    }

    void show_col(Col& c, const Window& w, int64_t now, bool fresh)
    {
        const int pct = pctNow(w, now);
        char buf[16]  = "--%";
        if (pct >= 0) {
            std::snprintf(buf, sizeof(buf), "%d%%", pct);
        }
        setText(c.pct, c.shown_pct, buf);
        const uint32_t color = fresh ? levelColor(_p, levelFor(pct)) : _p.dim;
        setTextColor(c.pct, c.shown_color, color);
        setBgColor(c.fill, c.shown_fill_color, color);
        setBarWidth(c.fill, c.shown_w, barWidth(pct, kColW, kBarH));
        setText(c.reset, c.shown_reset, resetShort(w.resetsAt, now));
    }

    // Four strokes through the middle: an eight-pointed spark
    void spark(int degrees, uint32_t color)
    {
        constexpr double kRad = 3.14159265358979 / 180.0;
        constexpr double kR   = 8.5;
        const double mid      = kSpark / 2.0;
        for (int i = 0; i < 4; ++i) {
            const double a = (degrees + i * 45) * kRad;
            const double c = std::cos(a) * kR;
            const double s = std::sin(a) * kR;
            Ray& ray       = _rays[i];
            ray.pts[0].x   = static_cast<lv_value_precise_t>(std::lround(mid - c));
            ray.pts[0].y   = static_cast<lv_value_precise_t>(std::lround(mid - s));
            ray.pts[1].x   = static_cast<lv_value_precise_t>(std::lround(mid + c));
            ray.pts[1].y   = static_cast<lv_value_precise_t>(std::lround(mid + s));
            lv_line_set_points(ray.line, ray.pts, 2);
            lv_obj_set_style_line_color(ray.line, rgb(color), 0);
        }
    }

    const Palette& _p;
    lv_obj_t* _spark       = nullptr;
    Ray _rays[4];
    lv_obj_t* _date        = nullptr;
    lv_obj_t* _state       = nullptr;
    lv_obj_t* _clock       = nullptr;
    lv_obj_t* _sweep_track = nullptr;
    lv_obj_t* _sweep       = nullptr;
    lv_obj_t* _note        = nullptr;
    lv_obj_t* _empty       = nullptr;
    Col _cols[2];
    std::string _shown_date;
    std::string _shown_clock;
    std::string _shown_state;
    std::string _shown_note;
    std::string _shown_empty;
    uint32_t _shown_state_color = 0;
    uint32_t _shown_spark_color = 0;
    uint32_t _shown_sweep_color = 0;
    int _shown_angle            = -1;
    int _shown_throb            = -1;
    int _shown_sweep_w          = -1;
};

}  // namespace

std::unique_ptr<StyleView> makeClaudeView(lv_obj_t* root)
{
    return std::make_unique<ClaudeView>(root);
}

}  // namespace stackpet::usage
