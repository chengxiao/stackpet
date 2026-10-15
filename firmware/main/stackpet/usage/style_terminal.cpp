/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
// StackPet screensaver style 终端: a green phosphor terminal. Scan lines over everything, a bright band sweeping down
// now and then, a blinking cursor after Claude's state; the windows as text bars in amber or red close to their limit
#include "clock_style.h"
#include <cctype>
#include <cstdio>

namespace stackpet::usage {

namespace {

using namespace widgets;

constexpr int kX           = 8;
constexpr int kLineY[5]    = {124, 146, 168, 190, 212};
constexpr int kCursorW     = 9;
constexpr int kCursorH     = 15;
constexpr int kBandH       = 20;
constexpr uint32_t kBandMs = 3200;  // down the screen ...
constexpr uint32_t kBandGap = 3800;  // ... and a pause
constexpr uint32_t kBlinkMs = 530;
constexpr uint32_t kThrob   = 600;

// Every third row darker: the scan lines, tiled over the screen (ARGB8888, alpha in the top byte)
const uint32_t kScanPixels[3] = {0x00000000, 0x00000000, 0x78000000};

void band_cb(void* var, int32_t v)
{
    lv_obj_set_y(static_cast<lv_obj_t*>(var), v);
}

std::string upper(std::string s)
{
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

// "WEEKLY [########--]  87% THU 15:00"
std::string window_line(const char* name, const Window& w, int64_t now, bool known)
{
    const int pct = known ? pctNow(w, now) : -1;
    std::string line = std::string(name) + " " + barText(pct) + " " + pctColumn(pct);
    const std::string reset = known ? resetEnglish(w.resetsAt, now) : "";
    return reset.empty() ? line : line + " " + reset;
}

class TerminalView : public StyleView {
public:
    explicit TerminalView(lv_obj_t* root) : _p(paletteFor(Style::Terminal))
    {
        _scan_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
        _scan_dsc.header.cf     = LV_COLOR_FORMAT_ARGB8888;
        _scan_dsc.header.w      = 1;
        _scan_dsc.header.h      = 3;
        _scan_dsc.header.stride = 4;
        _scan_dsc.data_size     = sizeof(kScanPixels);
        _scan_dsc.data          = reinterpret_cast<const uint8_t*>(kScanPixels);

        _prompt = label(root, &font_usage_mono_16, _p.dim);
        lv_label_set_text(_prompt, "$ claude --usage --watch");
        lv_obj_set_pos(_prompt, kX, 8);
        _clock = label(root, &font_usage_mono_72, _p.text);
        lv_obj_align(_clock, LV_ALIGN_TOP_MID, 0, 32);
        _date = label(root, &font_usage_mono_16, _p.muted);
        lv_obj_align(_date, LV_ALIGN_TOP_MID, 0, 90);
        for (int i = 0; i < 5; ++i) {
            _lines[i].obj = label(root, &font_usage_mono_16, _p.text);
            lv_obj_set_pos(_lines[i].obj, kX, kLineY[i]);
        }
        _cursor = plain(root);
        lv_obj_set_size(_cursor, kCursorW, kCursorH);
        lv_obj_set_style_bg_opa(_cursor, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(_cursor, rgb(_p.text), 0);

        // The sweeping band: a bright edge and a faint glow behind it
        _band = plain(root);
        lv_obj_set_size(_band, kScreenW, kBandH);
        lv_obj_set_style_bg_opa(_band, 22, 0);
        lv_obj_set_style_bg_color(_band, rgb(_p.text), 0);
        lv_obj_set_style_border_side(_band, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(_band, 2, 0);
        lv_obj_set_style_border_color(_band, rgb(_p.text), 0);
        lv_obj_set_style_border_opa(_band, 70, 0);
        lv_obj_set_y(_band, -kBandH);

        _scan = plain(root);  // last: over everything
        lv_obj_set_size(_scan, kScreenW, kScreenH);
        lv_obj_set_style_bg_image_src(_scan, &_scan_dsc, 0);
        lv_obj_set_style_bg_image_tiled(_scan, true, 0);
    }

    int floatX() const override
    {
        return 3;  // the longest line is 34 characters of 9 px
    }
    int floatY() const override
    {
        return 4;
    }

    void show(const ClockModel& m, bool night) override
    {
        const std::string note = feedNoteEnglish(m.status, m.haveSnapshot, m.ageMs);
        const bool fresh       = m.haveSnapshot && note.empty();
        setText(_clock, _shown_clock, clockText(m.local));
        setText(_date, _shown_date, dateEnglish(m.local));

        // The two windows; amber or red close to their limit, dim when old
        const int s_pct = m.haveSnapshot ? pctNow(m.snapshot.session, m.now) : -1;
        const int w_pct = m.haveSnapshot ? pctNow(m.snapshot.weekly, m.now) : -1;
        show_line(_lines[0], window_line("5-HOUR", m.snapshot.session, m.now, m.haveSnapshot),
                  fresh ? levelColor(_p, levelFor(s_pct)) : _p.muted);
        show_line(_lines[1], window_line("WEEKLY", m.snapshot.weekly, m.now, m.haveSnapshot),
                  fresh ? levelColor(_p, levelFor(w_pct)) : _p.muted);
        // Claude's state (or why there is no usage yet), the cursor after it
        const AgentState state  = fresh ? m.snapshot.state : AgentState::Unknown;
        const std::string what  = fresh ? stateEnglish(state, m.snapshot.waiting) : (m.haveSnapshot ? "--" : note);
        show_line(_lines[2], "STATUS " + (what.empty() ? std::string("--") : what),
                  state == AgentState::Wait ? _p.wait : _p.text);
        // Old usage: why; the model
        show_line(_lines[3], m.haveSnapshot && !note.empty() ? "LINK   " + note : "", _p.warn);
        show_line(_lines[4], fresh && !m.snapshot.model.empty() ? "MODEL  " + upper(m.snapshot.model) : "", _p.dim);

        lv_obj_update_layout(_lines[2].obj);
        const int cx = kX + lv_obj_get_width(_lines[2].obj) + 4;
        if (cx != _shown_cursor_x) {
            lv_obj_set_pos(_cursor, cx, kLineY[2] + 1);
            _shown_cursor_x = cx;
        }
        setBgColor(_cursor, _shown_cursor_color, state == AgentState::Wait ? _p.wait : _p.text);

        // Moving parts: none at night
        const int motion = night ? 0 : (state == AgentState::Wait ? 2 : 1);
        if (motion != _shown_motion) {
            if (motion == 0) {
                steady(_cursor, LV_OPA_COVER);
                steady(_lines[2].obj, LV_OPA_COVER);
                lv_anim_delete(_band, band_cb);
                setHidden(_band, true);
            } else {
                blink(_cursor, kBlinkMs);
                if (motion == 2) {
                    pulse(_lines[2].obj, LV_OPA_COVER, 90, kThrob);
                } else {
                    steady(_lines[2].obj, LV_OPA_COVER);
                }
                if (_shown_motion <= 0) {
                    setHidden(_band, false);
                    lv_anim_t a;
                    lv_anim_init(&a);
                    lv_anim_set_var(&a, _band);
                    lv_anim_set_exec_cb(&a, band_cb);
                    lv_anim_set_values(&a, -kBandH, kScreenH);
                    lv_anim_set_duration(&a, kBandMs);
                    lv_anim_set_repeat_delay(&a, kBandGap);
                    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
                    lv_anim_set_path_cb(&a, lv_anim_path_linear);
                    lv_anim_start(&a);
                }
            }
            _shown_motion = motion;
        }
    }

private:
    struct Line {
        lv_obj_t* obj = nullptr;
        std::string shown;
        uint32_t shown_color = 0;
    };

    void show_line(Line& l, const std::string& text, uint32_t color)
    {
        setText(l.obj, l.shown, text);
        setTextColor(l.obj, l.shown_color, color);
    }

    const Palette& _p;
    lv_image_dsc_t _scan_dsc{};
    lv_obj_t* _prompt = nullptr;
    lv_obj_t* _clock  = nullptr;
    lv_obj_t* _date   = nullptr;
    Line _lines[5];
    lv_obj_t* _cursor = nullptr;
    lv_obj_t* _band   = nullptr;
    lv_obj_t* _scan   = nullptr;
    std::string _shown_clock;
    std::string _shown_date;
    uint32_t _shown_cursor_color = 0;
    int _shown_cursor_x          = -1;
    int _shown_motion            = -1;
};

}  // namespace

std::unique_ptr<StyleView> makeTerminalView(lv_obj_t* root)
{
    return std::make_unique<TerminalView>(root);
}

}  // namespace stackpet::usage
