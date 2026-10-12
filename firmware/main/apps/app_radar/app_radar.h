/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <mooncake.h>
#include <cstdint>
#include <memory>

namespace stackpet::radar {
struct Contact;
struct RadarEvent;
struct RadarInput;
}  // namespace stackpet::radar

/**
 * @brief "RADAR": sweeps the room with the head, pings like a sonar and turns the screen into a radar scope.
 * A face (ESP-DL human_face_detect) seen in two detector runs in a row is locked on: the head turns to it, the
 * screen shows the camera with a red lock box, the LEDs flash red and a siren sounds for 3 s, then the robot
 * glares at the intruder with an angry face until it leaves. Petting the head calms it down.
 * See stackpet/face/README.md ("RADAR 应用").
 */
class AppRadar : public mooncake::AppAbility {
public:
    AppRadar();
    ~AppRadar();  // defined in the .cpp, where State is complete

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    struct State;

    void update_vision(uint32_t now, stackpet::radar::Contact& contact);
    void enter_motion_fallback(const char* reason);
    void handle_event(const stackpet::radar::RadarEvent& event, uint32_t now);
    void enter_phase_visuals(uint32_t now);
    void apply_head(uint32_t now);
    void rotate_yaw(int velocity);
    void watch_stall(uint32_t now);
    bool spin_stalled(uint32_t now);
    void handle_stall(uint32_t now);
    // Out of wheel mode for a position move to targetYaw: coast down, off the yaw sensor's dead band the shorter way
    // round, then position mode (false while still at it)
    bool leave_spin(uint32_t now, int targetYaw);
    void stop_spinning_now();  // closing: the same, blocking for a moment at most
    void turn_to_brake_point(int brakeAtYaw);  // a look-around stop: on until the sensor says the head got there
    void note_look_stop();
    void update_spin_inputs(uint32_t now, stackpet::radar::RadarInput& input);
    void update_spin_safety(uint32_t now);
    void update_screen(uint32_t now);
    void update_leds(uint32_t now);
    void update_scanner_leds(uint32_t now);
    void update_status();
    void set_avatar_visible(bool visible);

    std::unique_ptr<State> _state;
};
