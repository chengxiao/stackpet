/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
// Host test stand-in for main/hal/hal.h: only what stackpet/remote/remote_link.cpp uses (ESP-NOW and the clock)
#pragma once
#include <cstdint>
#include <functional>
#include <vector>

struct HostEspNowSignal {
    std::vector<std::function<void(const std::vector<uint8_t>&)>> slots;
    void connect(std::function<void(const std::vector<uint8_t>&)> slot)
    {
        slots.push_back(std::move(slot));
    }
    void emit(const std::vector<uint8_t>& data)
    {
        for (auto& slot : slots) {
            slot(data);
        }
    }
};

struct HostHal {
    HostEspNowSignal onEspNowData;
    uint32_t now      = 0;
    bool started      = false;
    int channel       = 0;
    int rx_channel    = 0;
    int channel_sets  = 0;
    int starts        = 0;
    int stops         = 0;

    uint32_t millis() const
    {
        return now;
    }
    void startEspNow(int ch)
    {
        starts++;
        started = true;
        channel = ch < 1 ? 1 : (ch > 13 ? 13 : ch);
    }
    void stopEspNow()
    {
        stops++;
        started = false;
        channel = 0;
    }
    void setEspNowChannel(int ch)
    {
        if (started) {
            channel_sets++;
            channel = ch < 1 ? 1 : (ch > 13 ? 13 : ch);
        }
    }
    int getEspNowChannel() const
    {
        return started ? channel : 0;
    }
    int getEspNowRxChannel() const
    {
        return rx_channel;
    }
    // An app's WiFi (wifi_up: connected to its router, on ap_channel): ESP-NOW on it
    bool wifi_up   = false;
    int ap_channel = 6;
    int attaches   = 0;
    bool attachEspNow()
    {
        if (started) {
            return true;
        }
        if (!wifi_up) {
            return false;
        }
        attaches++;
        started = true;
        channel = ap_channel;
        return true;
    }
    // The radio hands a packet to the receive callback; sentOn: the frame header's channel (the sender's own)
    void receive(const std::vector<uint8_t>& data, int sentOn)
    {
        rx_channel = sentOn;
        onEspNowData.emit(data);
    }
};

inline HostHal& GetHAL()
{
    static HostHal hal;
    return hal;
}
