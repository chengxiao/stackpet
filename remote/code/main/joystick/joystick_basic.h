/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef _JOYSTICK_BASIC_H_
#define _JOYSTICK_BASIC_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int channel;
    int id;
    int8_t bat;
    uint16_t joyX;
    uint16_t joyY;
    uint8_t screen_mode;
    uint8_t select_mode;
    bool btnB_status;
    bool home_held;       // StackPet: the M5 button (BtnA) held down, from the main loop ...
    uint32_t home_click;  // ... and pressed since a sending loop last looked (atomic: the main loop sets, it takes)
    uint8_t buttons;      // StackPet: bit 0 = the stick pressed in, bit 1 = the M5 button (the packet's 9th byte)
    uint8_t home_count;   // StackPet: the M5 button's presses so far (the 11th byte)
    float accel_x;
    float accel_y;
    float accel_z;

} joystick_data_t;

extern joystick_data_t joystick_data;

#ifdef __cplusplus
}
#endif

#endif