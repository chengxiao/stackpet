/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "remote_packet.h"
#include <algorithm>
#include <cmath>

namespace stackpet::remote {

namespace {

int16_t read_i16(const uint8_t* p)
{
    return static_cast<int16_t>(static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(p[1] << 8));
}

}  // namespace

std::optional<RemotePacket> parsePacket(const uint8_t* data, size_t size)
{
    if (data == nullptr || size < kPacketSize || size > kPacketSize + 3) {
        return std::nullopt;
    }
    RemotePacket packet;
    packet.target = data[0];
    packet.yaw    = read_i16(data + 1);
    packet.pitch  = read_i16(data + 3);
    packet.speed  = read_i16(data + 5);
    // The remote stays within these: anything else on the channel is not from it
    if (std::abs(packet.yaw) > 1800 || packet.pitch < -100 || packet.pitch > 1100 || packet.speed < 0 ||
        packet.speed > 1000 || data[7] > 1) {
        return std::nullopt;
    }
    // A stick that goes further than the remote's calibration: the edge
    packet.yaw   = std::clamp(packet.yaw, -1280, 1280);
    packet.pitch = std::clamp(packet.pitch, 0, 900);
    packet.laser = data[7] != 0;
    if (size >= kPacketSize + 1) {
        if ((data[8] & ~(kButtonStick | kButtonHome | kButtonEverywhere)) != 0) {
            return std::nullopt;
        }
        packet.hasButtons = true;
        packet.buttons    = data[8];
    }
    if (size >= kPacketSize + 2) {
        if (data[9] < 1 || data[9] > 14) {
            return std::nullopt;
        }
        packet.channel = data[9];
    }
    if (size == kPacketSize + 3) {
        packet.homeCount = data[10];
    }
    return packet;
}

bool isForReceiver(const RemotePacket& packet, int receiverId)
{
    return packet.target == 0 || packet.target == receiverId;
}

float stickDeflection(const RemotePacket& packet)
{
    const float x = static_cast<float>(packet.yaw) / 1280.0f;
    const float y = (static_cast<float>(packet.pitch) - 450.0f) / 450.0f;
    return std::min(std::max(std::fabs(x), std::fabs(y)), 1.0f);
}

}  // namespace stackpet::remote
