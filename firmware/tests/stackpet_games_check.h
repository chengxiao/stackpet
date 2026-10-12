/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
// Minimal assertion helpers for the StackPet games host tests (same style as motion_math_test.cpp)
#pragma once
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace stackpet_test {

inline int& checks()
{
    static int count = 0;
    return count;
}

inline void fail(const char* file, int line, const char* expr)
{
    std::cerr << file << ":" << line << ": check failed: " << expr << '\n';
    std::exit(1);
}

}  // namespace stackpet_test

#define CHECK(expr)                                              \
    do {                                                         \
        stackpet_test::checks()++;                               \
        if (!(expr)) stackpet_test::fail(__FILE__, __LINE__, #expr); \
    } while (0)

#define CHECK_NEAR(a, b, eps) CHECK(std::fabs((a) - (b)) <= (eps))

#define RUN_TEST(fn)                                                       \
    do {                                                                   \
        fn();                                                              \
        std::cout << "[ok] " << #fn << '\n';                               \
    } while (0)
