/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
// Host test stand-in for mooncake_log.h: the log goes nowhere, but the last info line is kept: its format string and
// its integer values (the tests look at which line was logged, and with what number)
#pragma once
#include <string>
#include <type_traits>
#include <vector>

namespace mclog {
inline std::string& lastInfo()
{
    static std::string line;
    return line;
}
inline std::vector<long long>& lastInfoValues()
{
    static std::vector<long long> values;
    return values;
}
template <typename T>
void keepInfoValue(const T& value)
{
    if constexpr (std::is_integral_v<T>) {
        lastInfoValues().push_back(static_cast<long long>(value));
    }
}
template <typename Tag, typename... Args>
void tagInfo(Tag&&, const char* format, Args&&... args)
{
    lastInfo() = format;
    lastInfoValues().clear();
    (keepInfoValue(args), ...);
}
template <typename... Args>
void tagWarn(Args&&...)
{
}
}  // namespace mclog
