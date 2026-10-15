/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
// StackPet screensaver style 表盘: two rings round the clock, the weekly window outside, the 5-hour one inside (in the
// color of how close each is to its limit), a dot going round the outer ring once a minute; the numbers and reset
// times in a column on the right
#include "clock_style.h"
#include <stackpet/ui/ui.h>
#include <cmath>
#include <cstdio>

namespace stackpet::usage {

namespace {

using namespace widgets;

constexpr int kCx        = 116;
constexpr int kCy        = 120;
constexpr int kOuterD    = 220;
constexpr int kOuterW    = 8;
constexpr int kInnerD    = 192;
constexpr int kInnerW    = 10;
constexpr int kComet     = 8;
constexpr int kPanelX    = 238;
constexpr uint32_t kThrob = 700;

void value_cb(void* var, int32_t v)
{
    lv_arc_set_value(static_cast<lv_obj_t*>(var), v);
}

lv_obj_t* ring(lv_obj_t* root, int d, int w, uint32_t track)
{
    lv_obj_t* arc = lv_arc_create(root);
    lv_obj_remove_style_all(arc);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(arc, d, d);
    lv_obj_set_pos(arc, kCx - d / 2, kCy - d / 2);
    lv_arc_set_rotation(arc, 270);  // from twelve o'clock, clockwise
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 0);
    for (lv_part_t part : {LV_PART_MAIN, LV_PART_INDICATOR}) {
        lv_obj_set_style_arc_width(arc, w, part);
        lv_obj_set_style_arc_rounded(arc, true, part);
        lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, part);
    }
    lv_obj_set_style_arc_color(arc, rgb(track), LV_PART_MAIN);
    return arc;
}

class RingsView : public StyleView {
public:
    explicit RingsView(lv_obj_t* root) : _p(paletteFor(Style::Rings))
    {
        const lv_font_t* cn = ui::fontCn20();
        _outer.arc          = ring(root, kOuterD, kOuterW, _p.track);
        _inner.arc          = ring(root, kInnerD, kInnerW, _p.track);
        _comet              = dot(root, kComet, _p.text);
        lv_obj_set_style_opa(_comet, 210, 0);
        // Inside the rings, centered on them
        const int dx = kCx - kScreenW / 2;
        _date        = label(root, cn, _p.muted);
        lv_obj_align(_date, LV_ALIGN_TOP_MID, dx, kCy - 54);
        _clock = label(root, &font_usage_clock_52, _p.text);
        lv_obj_align(_clock, LV_ALIGN_TOP_MID, dx, kCy - 22);
        _state = label(root, cn, _p.muted);
        lv_obj_align(_state, LV_ALIGN_TOP_MID, dx, kCy + 26);
        // The column on the right
        build_side(_session_side, root, "5 小时", 24);
        build_side(_weekly_side, root, "本周", 126);
    }

    int floatX() const override
    {
        return 5;
    }
    int floatY() const override
    {
        return 6;
    }

    void show(const ClockModel& m, bool night) override
    {
        const std::string reason = feedReason(m.status, m.haveSnapshot, m.ageMs);
        const bool fresh         = m.haveSnapshot && reason.empty();
        setText(_date, _shown_date, dateText(m.local));
        setText(_clock, _shown_clock, clockText(m.local));

        // The rings: no usage, nothing on them; old usage, dimmed
        show_ring(_outer, m.haveSnapshot ? pctNow(m.snapshot.weekly, m.now) : -1, fresh);
        show_ring(_inner, m.haveSnapshot ? pctNow(m.snapshot.session, m.now) : -1, fresh);
        show_side(_session_side, m.haveSnapshot ? m.snapshot.session : Window{}, m.now, fresh);
        show_side(_weekly_side, m.haveSnapshot ? m.snapshot.weekly : Window{}, m.now, fresh);

        // Under the clock: what Claude is doing, or why the usage is old or missing
        const AgentState state = fresh ? m.snapshot.state : AgentState::Unknown;
        setText(_state, _shown_state, fresh ? stateText(state, m.snapshot.waiting) : reason);
        setTextColor(_state, _shown_state_color, fresh && state != AgentState::Idle ? stateColor(_p, state) : _p.muted);
        const int throb = state == AgentState::Wait && !night ? 1 : 0;
        if (throb != _shown_throb) {
            if (throb) {
                pulse(_state, LV_OPA_COVER, 80, kThrob);
            } else {
                steady(_state, LV_OPA_COVER);
            }
            _shown_throb = throb;
        }

        // The dot round the outer ring: the second hand (amber while Claude waits)
        const double sec = clockValid(m.local) ? m.local.tm_sec + (m.ms % 1000) / 1000.0 : 0.0;
        const double a   = sec / 60.0 * 6.283185307179586;
        const double r   = kOuterD / 2.0 - kOuterW / 2.0;
        const int x      = static_cast<int>(std::lround(kCx + r * std::sin(a))) - kComet / 2;
        const int y      = static_cast<int>(std::lround(kCy - r * std::cos(a))) - kComet / 2;
        if (x != _shown_x || y != _shown_y) {
            lv_obj_set_pos(_comet, x, y);
            _shown_x = x;
            _shown_y = y;
        }
        setBgColor(_comet, _shown_comet_color, state == AgentState::Wait ? _p.wait : _p.text);
    }

private:
    struct Ring {
        lv_obj_t* arc        = nullptr;
        int shown_value      = -1;
        uint32_t shown_color = 0;
    };
    struct Side {
        lv_obj_t* name   = nullptr;
        lv_obj_t* pct    = nullptr;
        lv_obj_t* reset  = nullptr;  // "4:12 后", or the weekday of "周四 15:00" ...
        lv_obj_t* reset2 = nullptr;  // ... and its time on the next line (the column is narrow)
        std::string shown_pct;
        std::string shown_reset;
        std::string shown_reset2;
        uint32_t shown_color = 0;
    };

    void build_side(Side& s, lv_obj_t* root, const char* name, int y)
    {
        const lv_font_t* cn = ui::fontCn20();
        s.name              = label(root, cn, _p.muted);
        lv_label_set_text(s.name, name);
        lv_obj_set_pos(s.name, kPanelX, y);
        s.pct = label(root, &font_usage_num_26, _p.normal);
        lv_obj_set_pos(s.pct, kPanelX, y + 26);
        s.reset = label(root, cn, _p.muted);
        lv_obj_set_pos(s.reset, kPanelX, y + 54);
        s.reset2 = label(root, cn, _p.muted);
        lv_obj_set_pos(s.reset2, kPanelX, y + 78);
    }

    void show_ring(Ring& r, int pct, bool fresh)
    {
        const int value = std::max(pct, 0);
        if (value != r.shown_value) {
            if (r.shown_value < 0) {
                lv_arc_set_value(r.arc, value);
            } else {
                lv_anim_delete(r.arc, value_cb);
                lv_anim_t a;
                lv_anim_init(&a);
                lv_anim_set_var(&a, r.arc);
                lv_anim_set_exec_cb(&a, value_cb);
                lv_anim_set_values(&a, r.shown_value, value);
                lv_anim_set_duration(&a, 800);
                lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
                lv_anim_start(&a);
            }
            r.shown_value = value;
        }
        const uint32_t color = fresh ? levelColor(_p, levelFor(pct)) : _p.dim;
        if (color != r.shown_color) {
            lv_obj_set_style_arc_color(r.arc, rgb(color), LV_PART_INDICATOR);
            r.shown_color = color;
        }
    }

    void show_side(Side& s, const Window& w, int64_t now, bool fresh)
    {
        const int pct = pctNow(w, now);
        char buf[16]  = "--%";
        if (pct >= 0) {
            std::snprintf(buf, sizeof(buf), "%d%%", pct);
        }
        setText(s.pct, s.shown_pct, buf);
        setTextColor(s.pct, s.shown_color, fresh ? levelColor(_p, levelFor(pct)) : _p.dim);
        // "周四 15:00" on two lines; the rest fits on one
        const std::string reset = resetShort(w.resetsAt, now);
        const size_t space      = reset.rfind(' ');
        const bool split        = space != std::string::npos && reset.find(':', space) != std::string::npos;
        setText(s.reset, s.shown_reset, split ? reset.substr(0, space) : reset);
        setText(s.reset2, s.shown_reset2, split ? reset.substr(space + 1) : "");
    }

    const Palette& _p;
    Ring _outer;
    Ring _inner;
    Side _session_side;
    Side _weekly_side;
    lv_obj_t* _comet = nullptr;
    lv_obj_t* _date  = nullptr;
    lv_obj_t* _clock = nullptr;
    lv_obj_t* _state = nullptr;
    std::string _shown_date;
    std::string _shown_clock;
    std::string _shown_state;
    uint32_t _shown_state_color = 0;
    uint32_t _shown_comet_color = 0;
    int _shown_throb            = -1;
    int _shown_x                = -1000;
    int _shown_y                = -1000;
};

}  // namespace

std::unique_ptr<StyleView> makeRingsView(lv_obj_t* root)
{
    return std::make_unique<RingsView>(root);
}

}  // namespace stackpet::usage
