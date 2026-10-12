/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

namespace stackpet {

// Launcher index of each StackPet app (= install order in main.cpp, launcher excluded).
// Official apps occupy 0-6, see the requestWarmReboot() calls in apps/*.
constexpr int kAppIndexRemote = 7;
constexpr int kAppIndexFreeze = 8;
constexpr int kAppIndexSpin   = 9;
constexpr int kAppIndexFace   = 10;
constexpr int kAppIndexRadar  = 11;

}  // namespace stackpet
