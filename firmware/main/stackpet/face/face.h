/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stackchan/avatar/avatar/avatar.h>
#include <smooth_lvgl.hpp>
#include <lvgl.h>
#include <cstdint>
#include <memory>

namespace stackpet::face {

/**
 * Face styles. Every app creates its avatar through create(), so the style chosen in the FACE app applies
 * everywhere (AI agent, AVATAR, DANCE, games, ...). All styles implement the same Avatar / Feature interface, so
 * modifiers (blink, breath, speaking, idle motion, head pet) and JSON avatar control keep working.
 */
enum class Style : uint8_t {
    Official = 0,  // the original M5 StackChan face (dot eyes + line mouth)
    Classic,       // "经典加强": classic face with brows, emotion mouths and color accents
    BigEyes,       // "大眼萌系": big glowing eyes, emotion colors (EMO / Vector like)
    Count,
};

const char* styleName(Style style);  // UTF-8 display name, e.g. "大眼萌系"

// Persisted choice (NVS namespace "stackpet_face", key "style"); defaults to BigEyes. Cached after first read.
Style style();
// Main loop only (writes NVS).
void setStyle(Style style);

// Create and init an avatar of the given style on parent. font is the speech bubble font (same meaning as
// DefaultAvatar::init). panel (optional) receives the full screen container, e.g. for click handlers.
std::unique_ptr<stackchan::avatar::Avatar> create(Style style, lv_obj_t* parent,
                                                  const lv_font_t* font = &lv_font_montserrat_16,
                                                  uitk::lvgl_cpp::Container** panel = nullptr);

inline std::unique_ptr<stackchan::avatar::Avatar> create(lv_obj_t* parent, const lv_font_t* font = &lv_font_montserrat_16,
                                                         uitk::lvgl_cpp::Container** panel = nullptr)
{
    return create(style(), parent, font, panel);
}

}  // namespace stackpet::face
