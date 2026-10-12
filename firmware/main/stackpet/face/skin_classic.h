/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stackchan/avatar/avatar/avatar.h>
#include <smooth_lvgl.hpp>
#include <lvgl.h>
#include <memory>

namespace stackpet::face {

/**
 * "经典加强": the StackChan dot eyes + line mouth, plus brows, a mouth shape per emotion and a few color accents
 * (anger mark, tear, blush, hearts). Eyes and mouth are vector drawn (classic_features.*), the speech bubble is
 * the official one. Same public surface as stackchan::avatar::DefaultAvatar.
 */
class ClassicAvatar : public stackchan::avatar::Avatar {
public:
    void init(lv_obj_t* parent, const lv_font_t* font = &lv_font_montserrat_16);
    uitk::lvgl_cpp::Container* getPanel() const;

private:
    uitk::lvgl_cpp::Container* _panel = nullptr;  // owned by the speech bubble element, see skin_classic.cpp
};

}  // namespace stackpet::face
