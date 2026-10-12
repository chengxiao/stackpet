/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

namespace stackchan::avatar {

enum class Emotion {
    Neutral = 0,
    Happy,
    Angry,
    Sad,
    Doubt,
    Sleepy,
    // StackPet: appended so the values above keep their numbers (they travel over BLE / websocket JSON)
    Laughing,
    Surprised,
    Shy,
    Love,
};

constexpr int kEmotionCount = static_cast<int>(Emotion::Love) + 1;

}  // namespace stackchan::avatar
