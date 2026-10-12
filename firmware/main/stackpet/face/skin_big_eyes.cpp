/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "skin_big_eyes.h"
#include "bigeyes_features.h"
#include <stackchan/avatar/skins/default/default.h>

using namespace uitk::lvgl_cpp;
using namespace stackchan::avatar;

namespace stackpet::face {

BigEyesAvatar::~BigEyesAvatar()
{
    // The features delete their own objects and timers: let them go while the panel still exists, the panel after
    _key_elements.speechBubble.reset();
    _key_elements.mouth.reset();
    _key_elements.rightEye.reset();
    _key_elements.leftEye.reset();
}

void BigEyesAvatar::init(lv_obj_t* parent, const lv_font_t* font)
{
    _panel = std::make_unique<Container>(parent);
    _panel->align(LV_ALIGN_CENTER, 0, 0);
    _panel->setSize(320, 240);
    _panel->setRadius(0);
    _panel->setBorderWidth(0);
    _panel->setPadding(0, 0, 0, 0);  // the features place their objects in panel pixels
    _panel->setBgColor(lv_color_black());
    _panel->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    _key_elements.leftEye  = std::make_unique<bigeyes::BigEyesEye>(_panel->get(), true);
    _key_elements.rightEye = std::make_unique<bigeyes::BigEyesEye>(_panel->get(), false);
    _key_elements.mouth    = std::make_unique<bigeyes::BigEyesMouth>(_panel->get());
    // Created last, so it is drawn above the eyes and the mouth
    _key_elements.speechBubble =
        std::make_unique<DefaultSpeechBubble>(_panel->get(), lv_color_white(), lv_color_black(), font);
}

Container* BigEyesAvatar::getPanel() const
{
    return _panel.get();
}

}  // namespace stackpet::face
