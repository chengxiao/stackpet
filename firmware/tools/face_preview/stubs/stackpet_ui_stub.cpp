// Host stub for stackpet::ui: no assets partition here, so the full CJK 20 px font falls back to Montserrat 20
#include <stackpet/ui/ui.h>

LV_FONT_DECLARE(font_stackpet_title_28);
LV_FONT_DECLARE(font_stackpet_title_44);

namespace stackpet::ui {

const lv_font_t* fontCn20()
{
    return &font_stackpet_title_28;
}
const lv_font_t* fontTitle28()
{
    return &font_stackpet_title_28;
}
const lv_font_t* fontTitle44()
{
    return &font_stackpet_title_44;
}

}  // namespace stackpet::ui
