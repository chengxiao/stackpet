/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
// Host test stand-in for main/hal/hal.h: only the clock that stackchan/motion/servo.cpp reads
#pragma once
#include <cstdint>

struct HostHal {
    uint32_t now = 0;
    uint32_t millis() const
    {
        return now;
    }
};

inline HostHal& GetHAL()
{
    static HostHal hal;
    return hal;
}
