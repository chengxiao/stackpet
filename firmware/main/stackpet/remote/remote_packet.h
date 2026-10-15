/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>

namespace stackpet::remote {

// The StackChan remote's ESP-NOW packet (StickC-Plus + Hat Mini JoyC, firmware in remote/code), little endian:
//   [0] target id (0: every robot)   [1-2] yaw -1280..1280   [3-4] pitch 0..900   [5-6] speed 0..1000
//   [7] laser on/off (the remote's B button toggles it)
// The remote maps the stick's raw 0..4095 onto yaw and pitch without a limit, so a stick that goes further than its
// calibration sends up to yaw -1581..1800 and pitch -88..1081: parsed, they are back within the range.
// StackPet's remote firmware adds three (the official one sends 8 bytes):
//   [8] buttons, held down: bit 0 = the stick pressed in, bit 1 = the M5 button (the big one on the remote's front);
//       bit 2: this one is the M5 press's copy, sent on every channel (for a robot whose app has it on its router's
//       channel): it says nothing of where the remote is
//   [9] the WiFi channel the remote sends on, 1..14 (the same as sentOn below: a cross-check)
//   [10] the M5 button's presses so far (wraps round): one press however many copies of it come
struct RemotePacket {
    uint8_t target  = 0;
    int yaw         = 0;
    int pitch       = 0;
    int speed       = 0;
    bool laser      = false;
    bool hasButtons = false;
    uint8_t buttons = 0;
    int channel     = 0;  // the remote's own, 0: not said
    int homeCount   = -1;  // [10], -1: not said
    // RemoteLink: the channel the remote sent it on, from the espnow component's frame header (it puts that into
    // rx_ctrl->channel), also when the robot heard it a few channels off
    int sentOn = 0;
};

constexpr uint8_t kButtonStick = 0x01;
constexpr uint8_t kButtonHome  = 0x02;  // the M5 button: back to the home screen (RemoteLink::takeHomePress())
constexpr uint8_t kButtonEverywhere = 0x04;  // the M5 press's copy on every channel
constexpr size_t kPacketSize   = 8;
// Pressed in with the stick further off the middle than this: pushed too hard, not meant as a press
constexpr float kPressDeflection = 0.5f;

// The packet, or nothing for anything that is not one (other ESP-NOW traffic on the channel)
std::optional<RemotePacket> parsePacket(const uint8_t* data, size_t size);

// Meant for the robot with this receiver id (or for every robot)
bool isForReceiver(const RemotePacket& packet, int receiverId);

// The stick off its rest position (0: centre, 1: the edge), either axis
float stickDeflection(const RemotePacket& packet);

}  // namespace stackpet::remote
