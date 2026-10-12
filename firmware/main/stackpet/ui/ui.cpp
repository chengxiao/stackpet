/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "ui.h"
#include <assets.h>
#include <cbin_font.h>
#include <mooncake_log.h>
#include <string_view>

LV_FONT_DECLARE(font_stackpet_title_28);
LV_FONT_DECLARE(font_stackpet_title_44);

namespace stackpet::ui {

static const std::string_view _tag = "StackPetUI";

// Packed into the assets partition by build_default_assets.py: the builtin text font of this board is
// font_puhui_basic_20_4, for which the script ships the full "common" variant as a cbin font.
static constexpr const char* kCn20AssetName = "font_puhui_common_20_4.bin";

const lv_font_t* fontCn20()
{
    static const lv_font_t* font = nullptr;
    if (font != nullptr) {
        return font;
    }

    void* data  = nullptr;
    size_t size = 0;
    if (Assets::GetInstance().GetAssetData(kCn20AssetName, data, size) && data != nullptr && size > 0) {
        // The glyph data stays in the memory-mapped partition, only the small descriptors are copied
        lv_font_t* cbin = cbin_font_create(static_cast<uint8_t*>(data));
        if (cbin != nullptr) {
            // LV_SYMBOL_* icons and anything the subset lacks come from the built-in Montserrat
            cbin->fallback = &lv_font_montserrat_20;
            font           = cbin;
            mclog::tagInfo(_tag, "loaded {} ({} bytes)", kCn20AssetName, size);
        }
    }

    if (font == nullptr) {
        mclog::tagWarn(_tag, "{} not found in assets, falling back to Montserrat 20", kCn20AssetName);
        font = &lv_font_montserrat_20;
    }
    return font;
}

// The generated fonts are const, so use a mutable copy to attach the Chinese 20 px font as fallback:
// a character missing from the subset (fonts not regenerated yet) then still renders, just smaller.
static const lv_font_t* with_cn_fallback(const lv_font_t& generated, lv_font_t& storage, bool& ready)
{
    if (!ready) {
        storage          = generated;
        storage.fallback = fontCn20();
        ready            = true;
    }
    return &storage;
}

const lv_font_t* fontTitle28()
{
    static lv_font_t storage;
    static bool ready = false;
    return with_cn_fallback(font_stackpet_title_28, storage, ready);
}

const lv_font_t* fontTitle44()
{
    static lv_font_t storage;
    static bool ready = false;
    return with_cn_fallback(font_stackpet_title_44, storage, ready);
}

}  // namespace stackpet::ui
