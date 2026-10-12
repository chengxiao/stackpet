/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "face.h"
#include <settings.h>

namespace stackpet::face {

static constexpr const char* kNvsNamespace = "stackpet_face";
static constexpr const char* kNvsKey       = "style";
static constexpr Style kDefaultStyle       = Style::BigEyes;

static bool _cached      = false;
static Style _cached_style = kDefaultStyle;

Style style()
{
    if (!_cached) {
        Settings settings(kNvsNamespace, false);
        int value = settings.GetInt(kNvsKey, static_cast<int>(kDefaultStyle));
        _cached_style =
            value >= 0 && value < static_cast<int>(Style::Count) ? static_cast<Style>(value) : kDefaultStyle;
        _cached = true;
    }
    return _cached_style;
}

void setStyle(Style style)
{
    if (static_cast<int>(style) >= static_cast<int>(Style::Count)) {
        return;
    }
    Settings settings(kNvsNamespace, true);
    settings.SetInt(kNvsKey, static_cast<int>(style));
    _cached_style = style;
    _cached       = true;
}

}  // namespace stackpet::face
