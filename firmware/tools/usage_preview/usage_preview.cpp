// Host renderer for the launcher's screensaver (stackpet/usage/clock_screen.cpp): every style in each scene to a PPM,
// exactly as LVGL draws it on the 320x240 CoreS3 screen (at full backlight: the device dims it while the screensaver
// shows). See render.sh. A snapshot JSON as the second argument (ClaudeUsageBar's GET /api/v1/snapshot) adds a scene
// with it, at its own time
#include <lvgl.h>
#include <stackpet/usage/clock_screen.h>
#include <stackpet/usage/usage_snapshot.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace stackpet::usage;

static uint32_t _ms = 0;
static constexpr int kW = 320, kH = 240;
static uint16_t _fb[kW * kH];
alignas(64) static uint16_t _draw_buf[kW * kH];

static void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px)
{
    const uint16_t* src = reinterpret_cast<const uint16_t*>(px);
    for (int y = area->y1; y <= area->y2; y++) {
        for (int x = area->x1; x <= area->x2; x++) {
            _fb[y * kW + x] = *src++;
        }
    }
    lv_display_flush_ready(disp);
}

static void write_ppm(const std::string& path)
{
    FILE* f = fopen(path.c_str(), "wb");
    fprintf(f, "P6\n%d %d\n255\n", kW, kH);
    for (int i = 0; i < kW * kH; i++) {
        const uint16_t c = _fb[i];
        const uint8_t rgb[3] = {static_cast<uint8_t>(((c >> 11) & 0x1F) * 255 / 31),
                                static_cast<uint8_t>(((c >> 5) & 0x3F) * 255 / 63),
                                static_cast<uint8_t>((c & 0x1F) * 255 / 31)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void run(lv_display_t* disp, int ms)
{
    for (int t = 0; t < ms; t += 10) {
        _ms += 10;
        lv_timer_handler();
    }
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(disp);
}

static std::tm local_of(int64_t t)
{
    const std::time_t tt = static_cast<std::time_t>(t);
    std::tm tm{};
    localtime_r(&tt, &tm);
    return tm;
}

// Tue 2026-10-13 19:55:56, UTC+8: the scenes' times are from there
static constexpr int64_t kBase = 1791892556;

static ClockModel scene(int64_t now, int sPct, int64_t sLeft, int wPct, int64_t wLeft, AgentState state,
                        int waiting = 0)
{
    ClockModel m;
    m.now              = now;
    m.local            = local_of(now);
    m.haveSnapshot     = true;
    m.status           = FeedStatus::Ok;
    m.snapshot.session = {sPct, sLeft > 0 ? now + sLeft : 0};
    m.snapshot.weekly  = {wPct, wLeft > 0 ? now + wLeft : 0};
    m.snapshot.state   = state;
    m.snapshot.waiting = waiting;
    m.snapshot.ts      = now;
    m.snapshot.model   = "Opus 5.5";
    return m;
}

struct Scene {
    std::string key;
    ClockModel model;
};

int main(int argc, char** argv)
{
    const std::string out = argc > 1 ? argv[1] : ".";
    setenv("TZ", "CST-8", 1);
    tzset();
    lv_init();
    lv_tick_set_cb([]() -> uint32_t { return _ms; });
    lv_display_t* disp = lv_display_create(kW, kH);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, _draw_buf, nullptr, sizeof(_draw_buf), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, flush_cb);

    const int64_t h         = 3600;
    const int64_t afternoon = kBase - 5 * h - 26 * 60 + 17;  // 14:30:13
    const int64_t morning   = kBase - 9 * h - 51 * 60 + 38;  // 10:05:34
    const int64_t late      = kBase + 3 * h + 24 * 60 + 9;   // 23:20:05

    std::vector<Scene> scenes = {
        {"work", scene(afternoon, 23, 2 * h + 15 * 60, 41, 3 * 86400 + 5 * h, AgentState::Work)},
        {"wait", scene(morning, 75, 1 * h + 40 * 60, 86, 43 * h, AgentState::Wait)},
        {"done", scene(kBase, 94, 37 * 60, 97, 20 * h, AgentState::Done)},
        {"night", scene(late, 12, 3 * h, 87, 39 * h, AgentState::Work)},
    };
    Scene stale{"stale", scene(afternoon, 23, 2 * h, 41, 3 * 86400, AgentState::Work)};
    stale.model.status = FeedStatus::Unreachable;
    stale.model.ageMs  = 12 * 60 * 1000;
    scenes.push_back(stale);
    Scene setup{"setup", scene(afternoon, 0, 0, 0, 0, AgentState::Unknown)};
    setup.model.haveSnapshot = false;
    setup.model.status       = FeedStatus::NotSetUp;
    scenes.push_back(setup);
    // The Mac's snapshot, as it is
    if (argc > 2) {
        std::ifstream f(argv[2]);
        std::stringstream ss;
        ss << f.rdbuf();
        ClockModel m;
        if (parseSnapshot(ss.str(), m.snapshot) && m.snapshot.ts > 0) {
            m.now          = m.snapshot.ts;
            m.local        = local_of(m.now);
            m.haveSnapshot = true;
            m.status       = FeedStatus::Ok;
            scenes.push_back({"real", m});
        } else {
            fprintf(stderr, "%s: not a snapshot\n", argv[2]);
        }
    }

    for (int s = 0; s < kStyleCount; ++s) {
        const Style style = static_cast<Style>(s);
        for (const Scene& sc : scenes) {
            ClockModel m = sc.model;
            m.ms         = 41000;  // a moment in: the float off center, the motion under way
            ClockScreen screen(style);
            screen.show(m);
            run(disp, 1600);  // the animations (the terminal's band halfway down)
            m.ms += 1600;
            screen.show(m);
            run(disp, 0);
            write_ppm(out + "/" + styleKey(style) + "_" + sc.key + ".ppm");
        }
        printf("%s (%s)\n", styleKey(style), styleName(style));
    }
    return 0;
}
