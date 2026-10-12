// Host renderer for the StackPet face styles: renders every style x emotion (plus blink / speaking / gaze frames)
// to PPM files exactly as LVGL draws them on the 320x240 CoreS3 screen. See render.sh.
#include <lvgl.h>
#include <stackpet/face/face.h>
#include <stackchan/avatar/avatar/avatar.h>
#include <stackchan/avatar/decorators/decorators.h>
#include <smooth_ui_toolkit.hpp>
#include <uitk/short_namespace.hpp>
#include <hal/hal.h>
#include <cstdio>
#include <string>
#include <vector>

using namespace stackchan::avatar;
using stackpet::face::Style;

static uint32_t _fake_ms = 0;
uint32_t face_preview_millis()
{
    return _fake_ms;
}
Hal& GetHAL()
{
    static Hal hal;
    return hal;
}

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
        uint16_t c = _fb[i];
        uint8_t rgb[3] = {static_cast<uint8_t>(((c >> 11) & 0x1F) * 255 / 31),
                          static_cast<uint8_t>(((c >> 5) & 0x3F) * 255 / 63), static_cast<uint8_t>((c & 0x1F) * 255 / 31)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

// Advance the fake clock and let both LVGL animations and the avatar's own update() settle
static void settle(Avatar& avatar, lv_display_t* disp, int ms)
{
    for (int t = 0; t < ms; t += 16) {
        _fake_ms += 16;
        avatar.update();
        lv_timer_handler();
    }
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(disp);
}

static const char* kEmotionKeys[] = {"neutral", "happy",    "angry",     "sad", "doubt",
                                     "sleepy",  "laughing", "surprised", "shy", "love"};
static const char* kStyleKeys[]   = {"official", "classic", "bigeyes"};

int main(int argc, char** argv)
{
    std::string out = argc > 1 ? argv[1] : ".";
    std::vector<Style> styles = {Style::Official, Style::Classic, Style::BigEyes};

    smooth_ui_toolkit::ui_hal::on_get_tick([]() { return _fake_ms; });
    lv_init();
    lv_tick_set_cb([]() -> uint32_t { return _fake_ms; });
    lv_display_t* disp = lv_display_create(kW, kH);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, _draw_buf, nullptr, sizeof(_draw_buf), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, flush_cb);

    for (Style style : styles) {
        const char* sk = kStyleKeys[static_cast<int>(style)];
        for (int e = 0; e < kEmotionCount; e++) {
            lv_obj_clean(lv_screen_active());
            auto avatar = stackpet::face::create(style, lv_screen_active());
            settle(*avatar, disp, 200);
            avatar->setEmotion(static_cast<Emotion>(e));
            settle(*avatar, disp, 1500);
            write_ppm(out + "/" + sk + "_" + kEmotionKeys[e] + ".ppm");
            avatar.reset();
        }
        // Behaviour frames on the neutral face: blink, speaking, gaze
        struct Frame {
            const char* key;
            void (*apply)(Avatar&);
        };
        Frame frames[] = {
            {"x_blink", [](Avatar& a) { a.leftEye().setWeight(0); a.rightEye().setWeight(0); }},
            {"x_speaking", [](Avatar& a) { a.mouth().setWeight(70); }},
            {"x_look_left", [](Avatar& a) { a.leftEye().setPosition({-100, 0}); a.rightEye().setPosition({-100, 0}); a.mouth().setPosition({-100, 0}); }},
            {"x_look_up", [](Avatar& a) { a.leftEye().setPosition({0, -100}); a.rightEye().setPosition({0, -100}); a.mouth().setPosition({0, -100}); }},
            {"x_speech", [](Avatar& a) { a.setSpeech("Hello!"); }},
            // Decorators the apps put on top of the faces, to check for overlaps: RADAR / FREEZE caught (angry),
            // FREEZE / SPIN results (love, shy, laughing + sweat), FREEZE win, head pet, SPIN spin-up
            {"d_angry", [](Avatar& a) {
                 a.setEmotion(Emotion::Angry);
                 a.addDecorator(std::make_unique<AngryDecorator>(lv_screen_active(), 0, 500));
             }},
            {"d_love", [](Avatar& a) {
                 a.setEmotion(Emotion::Love);
                 a.addDecorator(std::make_unique<HeartDecorator>(lv_screen_active(), 0, 500));
             }},
            {"d_shy", [](Avatar& a) {
                 a.setEmotion(Emotion::Shy);
                 a.addDecorator(std::make_unique<ShyDecorator>(lv_screen_active(), 0));
             }},
            {"d_laughing", [](Avatar& a) {
                 a.setEmotion(Emotion::Laughing);
                 a.addDecorator(std::make_unique<SweatDecorator>(lv_screen_active(), 0, 700));
             }},
            {"d_love_shy", [](Avatar& a) {
                 a.setEmotion(Emotion::Love);
                 a.addDecorator(std::make_unique<HeartDecorator>(lv_screen_active(), 0, 500));
                 a.addDecorator(std::make_unique<ShyDecorator>(lv_screen_active(), 0));
             }},
            {"d_head_pet", [](Avatar& a) {
                 a.setEmotion(Emotion::Happy);
                 a.addDecorator(std::make_unique<HeartDecorator>(lv_screen_active(), 0, 500));
                 a.addDecorator(std::make_unique<ShyDecorator>(lv_screen_active(), 0));
             }},
            {"d_dizzy", [](Avatar& a) {
                 a.setEmotion(Emotion::Happy);
                 a.addDecorator(std::make_unique<DizzyDecorator>(lv_screen_active(), 0, 300));
             }},
        };
        for (auto& frame : frames) {
            lv_obj_clean(lv_screen_active());
            auto avatar = stackpet::face::create(style, lv_screen_active());
            settle(*avatar, disp, 200);
            frame.apply(*avatar);
            settle(*avatar, disp, 800);
            write_ppm(out + "/" + sk + "_" + frame.key + ".ppm");
            avatar.reset();
        }
    }
    printf("rendered %d styles\n", static_cast<int>(styles.size()));
    return 0;
}
