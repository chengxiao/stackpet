/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
// No NVS / HAL here: this file is also compiled by the host face preview renderer (tools/face_preview)
#include "face.h"
#include "skin_classic.h"
#include "skin_big_eyes.h"
#include <stackchan/avatar/skins/default/default.h>

using namespace stackchan::avatar;

namespace stackpet::face {

const char* styleName(Style style)
{
    switch (style) {
        case Style::Official:
            return "官方原版";
        case Style::Classic:
            return "经典加强";
        case Style::BigEyes:
            return "大眼萌系";
        default:
            return "?";
    }
}

std::unique_ptr<Avatar> create(Style style, lv_obj_t* parent, const lv_font_t* font, uitk::lvgl_cpp::Container** panel)
{
    switch (style) {
        case Style::Classic: {
            auto avatar = std::make_unique<ClassicAvatar>();
            avatar->init(parent, font);
            if (panel) {
                *panel = avatar->getPanel();
            }
            return avatar;
        }
        case Style::BigEyes: {
            auto avatar = std::make_unique<BigEyesAvatar>();
            avatar->init(parent, font);
            if (panel) {
                *panel = avatar->getPanel();
            }
            return avatar;
        }
        case Style::Official:
        default: {
            auto avatar = std::make_unique<DefaultAvatar>();
            avatar->init(parent, font);
            if (panel) {
                *panel = avatar->getPanel();
            }
            return avatar;
        }
    }
}

}  // namespace stackpet::face
