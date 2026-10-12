/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "skin_classic.h"
#include "classic_features.h"
#include <stackchan/avatar/skins/default/default.h>
#include <memory>

using namespace uitk::lvgl_cpp;
using namespace stackchan::avatar;

namespace stackpet::face {

namespace {

/**
 * The official speech bubble, plus ownership of the avatar panel.
 *
 * Avatar has no virtual destructor, so deleting the avatar through std::unique_ptr<Avatar> (resetAvatar(), or the
 * FACE app switching styles) never runs ClassicAvatar's own destructor: a panel owned by ClassicAvatar would stay on
 * screen. Key elements are destroyed properly (Element has a virtual destructor) and the speech bubble goes first,
 * so the panel lives here (same approach as BigEyesAvatar). Deleting it also deletes the eye / mouth objects, which
 * ClassicFeature notices through LV_EVENT_DELETE.
 */
class PanelSpeechBubble : public SpeechBubble {
public:
    PanelSpeechBubble(std::unique_ptr<Container> panel, const lv_font_t* font) : _panel(std::move(panel))
    {
        // Created last, so it is drawn above the eyes and the mouth
        _bubble = std::make_unique<DefaultSpeechBubble>(_panel->get(), lv_color_white(), lv_color_black(), font);
    }
    ~PanelSpeechBubble() override
    {
        _bubble.reset();
        _panel.reset();
    }

    void setSpeech(std::string_view text) override
    {
        _bubble->setSpeech(text);
    }
    void clearSpeech() override
    {
        _bubble->clearSpeech();
    }
    void setVisible(bool visible) override
    {
        SpeechBubble::setVisible(visible);
        _bubble->setVisible(visible);
    }
    void setTextFont(void* font) override
    {
        _bubble->setTextFont(font);
    }

private:
    std::unique_ptr<Container> _panel;
    std::unique_ptr<DefaultSpeechBubble> _bubble;
};

}  // namespace

void ClassicAvatar::init(lv_obj_t* parent, const lv_font_t* font)
{
    auto panel = std::make_unique<Container>(parent);
    panel->align(LV_ALIGN_CENTER, 0, 0);
    panel->setSize(320, 240);
    panel->setRadius(0);
    panel->setBorderWidth(0);
    panel->setBgColor(lv_color_black());
    panel->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
    _panel = panel.get();

    _key_elements.leftEye      = std::make_unique<classic::ClassicEye>(_panel->get(), true);
    _key_elements.rightEye     = std::make_unique<classic::ClassicEye>(_panel->get(), false);
    _key_elements.mouth        = std::make_unique<classic::ClassicMouth>(_panel->get());
    _key_elements.speechBubble = std::make_unique<PanelSpeechBubble>(std::move(panel), font);
}

Container* ClassicAvatar::getPanel() const
{
    return _panel;
}

}  // namespace stackpet::face
