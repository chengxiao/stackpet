// Host stub for the face preview renderer: only what avatar/decorator code needs from the firmware HAL
#pragma once
#include <cstdint>
#include <lvgl.h>

uint32_t face_preview_millis();

class Hal {
public:
    uint32_t millis()
    {
        return face_preview_millis();
    }
    void delay(uint32_t) {}
};

Hal& GetHAL();

class LvglLockGuard {
public:
    LvglLockGuard() {}
};
