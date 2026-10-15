// Host stub for stackpet::ui: the CN 20 px font is the one render.sh makes from the same TTF (PuHuiTi common, 4 bpp)
#include <stackpet/ui/ui.h>

LV_FONT_DECLARE(font_preview_cn20);

namespace stackpet::ui {

const lv_font_t* fontCn20()
{
    return &font_preview_cn20;
}

}  // namespace stackpet::ui
