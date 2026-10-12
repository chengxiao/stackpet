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
 * "大眼萌系" (style B, see stackpet/face/README.md): big glowing rounded eyes whose shape and colour carry the
 * emotion (EMO / Vector like), no mouth at rest and a small mouth while talking. Same public surface as
 * stackchan::avatar::DefaultAvatar.
 */
class BigEyesAvatar : public stackchan::avatar::Avatar {
public:
    ~BigEyesAvatar();

    void init(lv_obj_t* parent, const lv_font_t* font = &lv_font_montserrat_16);
    uitk::lvgl_cpp::Container* getPanel() const;

private:
    std::unique_ptr<uitk::lvgl_cpp::Container> _panel;
};

}  // namespace stackpet::face
