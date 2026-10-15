/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "remote_link.h"
#include <hal/hal.h>
#include <mooncake_log.h>
#include <settings.h>
#include <algorithm>
#include <iterator>
#if defined(ESP_PLATFORM)
#include <esp_attr.h>
#endif

namespace stackpet::remote {

static constexpr const char* kTag          = "Remote";
static constexpr const char* kNvsNamespace = "stackpet_remote";
static constexpr const char* kNvsChannel   = "channel";
static constexpr const char* kNvsId        = "id";
static constexpr int kDefaultChannel       = 1;     // the remote's default
static constexpr int kDefaultReceiverId    = 1;     // the remote control app's default
static constexpr uint32_t kProbeEveryMs    = 3000;  // while looking: a look at one other channel this often ...
static constexpr uint32_t kProbeMs         = 150;   // ... this long (the remote sends every ~20 ms while it is used)
// A remote heard on another channel is followed at most this often (two remotes sending to every robot from
// different channels would pull it to and fro, writing NVS each time)
static constexpr uint32_t kFollowEveryMs   = 3000;
static constexpr size_t kQueueLimit        = 32;
static constexpr uint32_t kLogEveryMs      = 2000;  // a summary of the packets while they come (one per packet is debug)
static constexpr uint32_t kHomeFreshMs     = 1000;  // the M5 button's press counts this long (takeHomePress())
static constexpr std::string_view kListeningApps[] = {"ESPNOW.REMOTE", "RADAR", "FACE", "SPIN", "FREEZE"};
// SETUP connects only in some of its pages, DANCE uses BLE alone: the remote is not heard there
static constexpr std::string_view kWifiApps[] = {"AI.AGENT", "AVATAR", "APP.CENTER", "EZDATA", "WEB.REMOTE"};

// StackPet's remote heard since power-on (usedSincePowerOn()): RTC memory keeps it over warm reboots, a power cut
// leaves noise there, hence a mark rather than a flag. Set by the receive callback, read by the main loop
#if defined(ESP_PLATFORM)
static RTC_NOINIT_ATTR uint32_t s_used;
#else
static uint32_t s_used = 0;  // host tests: one power-on for the whole run
#endif
static constexpr uint32_t kUsedMark = 0x52454D55u;  // "REMU"

bool appKeepsListening(std::string_view appName)
{
    return std::find(std::begin(kListeningApps), std::end(kListeningApps), appName) != std::end(kListeningApps);
}

bool appListensOnItsWifi(std::string_view appName)
{
    return std::find(std::begin(kWifiApps), std::end(kWifiApps), appName) != std::end(kWifiApps);
}

// Ours, and from a channel this robot can be on (1..13: a remote on 14 is only heard a channel off, close to it)
static bool is_ours(const RemotePacket& packet, int receiverId)
{
    return isForReceiver(packet, receiverId) && packet.channel != 14 && packet.sentOn != 14;
}

static int saved_channel()
{
    Settings settings(kNvsNamespace, false);
    const int channel = settings.GetInt(kNvsChannel, kDefaultChannel);
    return channel >= 1 && channel <= 13 ? channel : kDefaultChannel;
}

RemoteLink& RemoteLink::instance()
{
    static RemoteLink link;
    return link;
}

void RemoteLink::connect_once()
{
    if (!_connected) {
        _connected = true;
        GetHAL().onEspNowData.connect([this](const std::vector<uint8_t>& data) { on_data(data); });
    }
}

void RemoteLink::forget_received()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _queue.clear();  // heard while not listening (the callback stays connected)
    _home_down    = false;
    _home_pressed = false;
    _home_count   = -1;
}

void RemoteLink::start(bool hop)
{
    connect_once();
    if (_listening) {
        return;
    }
    receiverId();  // read before anything comes in: the receive callback filters by it too
    _saved_channel = saved_channel();
    _channel       = _saved_channel;
    GetHAL().startEspNow(_channel);
    _channel   = GetHAL().getEspNowChannel();
    _listening     = true;
    _paused        = false;
    _hopping       = hop;
    _probing       = false;
    _probe_channel = _channel;
    _probe_ms      = GetHAL().millis();
    _followed      = false;
    _stick_pressed = false;
    forget_received();
    mclog::tagInfo(kTag, "listening on channel {}{}", _channel, hop ? ", now and then on the others too" : "");
}

void RemoteLink::attach()
{
    if (_listening || !usedSincePowerOn()) {
        return;
    }
    connect_once();
    receiverId();  // read before anything comes in: the receive callback filters by it too
    if (!GetHAL().attachEspNow()) {
        return;
    }
    _attached      = true;
    _hopping       = false;
    _probing       = false;
    _channel       = GetHAL().getEspNowChannel();
    _followed      = false;
    _stick_pressed = false;
    forget_received();
    _listening = true;  // last: the main loop may be asking already
    mclog::tagInfo(kTag, "listening on the app's WiFi, channel {}: the M5 button's copies come there", _channel);
}

void RemoteLink::release()
{
    _receiver_id = -1;  // read from NVS again next time
    if (!_listening) {
        return;
    }
    _listening = false;
    _attached  = false;
    _paused    = false;
    _hopping   = false;
    _probing   = false;
    GetHAL().stopEspNow();
    forget_received();
    _channel = 0;
    mclog::tagInfo(kTag, "released the WiFi");
}

bool RemoteLink::listening() const
{
    return _listening;
}

int RemoteLink::channel() const
{
    return _listening ? _channel : 0;
}

void RemoteLink::lockChannel(int channel)
{
    if (!_listening || _attached) {
        return;  // attached: the app's channel, the router's
    }
    _hopping = false;
    _probing = false;
    switch_channel(channel);
}

void RemoteLink::settle()
{
    if (!_listening || !_hopping) {
        return;  // not listening, or on the remote's channel (or one picked by hand) already
    }
    _hopping = false;
    _probing = false;
    switch_channel(_saved_channel);
}

void RemoteLink::pause()
{
    if (!_listening || _attached) {
        return;
    }
    _paused = true;
    if (_probing) {
        // Back where it stays (the fetch returns the radio there), not to the channel looked at
        _probing = false;
        _channel = _saved_channel;
    }
}

void RemoteLink::resume(uint32_t nowMs)
{
    if (!_paused) {
        return;
    }
    _paused   = false;
    _probe_ms = nowMs;
}

bool RemoteLink::paused() const
{
    return _paused;
}

void RemoteLink::switch_channel(int channel)
{
    if (channel != _channel) {
        GetHAL().setEspNowChannel(channel);
        _channel = GetHAL().getEspNowChannel();
    }
}

std::vector<RemotePacket> RemoteLink::update(uint32_t nowMs)
{
    std::vector<RemotePacket> packets;
    if (!_listening) {
        return packets;
    }
    {
        std::lock_guard<std::mutex> lock(_mutex);
        packets.swap(_queue);
    }
    const int id    = receiverId();
    const int heard = static_cast<int>(packets.size());
    packets.erase(
        std::remove_if(packets.begin(), packets.end(), [id](const RemotePacket& p) { return !is_ours(p, id); }),
        packets.end());
    _log_heard += heard;
    _log_ours += static_cast<int>(packets.size());
    if (!packets.empty()) {
        _log_last = packets.back();
    }
    if (_log_heard > 0 && nowMs - _log_ms >= kLogEveryMs) {
        mclog::tagInfo(kTag, "{} packets ({} for receiver {}) in {} ms; last: yaw {} pitch {} laser {} buttons {} channel {} "
                       "sent on {}",
                       _log_heard, _log_ours, id, nowMs - _log_ms, _log_last.yaw, _log_last.pitch, _log_last.laser ? 1 : 0,
                       _log_last.hasButtons ? static_cast<int>(_log_last.buttons) : -1, _log_last.channel,
                       _log_last.sentOn);
        _log_ms    = nowMs;
        _log_heard = 0;
        _log_ours  = 0;
    }
    // Where this robot's remote is: the channel in the frame header (the espnow component's, the sender's own),
    // else the one StackPet's remote says it sends on; they should be the same, a mismatch goes to the log once
    int said = 0;
    int sent = 0;
    for (const auto& p : packets) {
        said = p.channel >= 1 && p.channel <= 13 ? p.channel : said;
        sent = p.sentOn >= 1 && p.sentOn <= 13 ? p.sentOn : sent;
        if (p.channel != 0 && p.sentOn != 0 && p.channel != p.sentOn && !_mismatch_logged) {
            _mismatch_logged = true;
            mclog::tagWarn(kTag, "the remote says channel {}, its frame {}: the frame's", p.channel, p.sentOn);
        }
        const bool pressed = p.hasButtons && (p.buttons & kButtonStick) != 0;
        if (pressed != _stick_pressed) {
            _stick_pressed = pressed;
            mclog::tagInfo(kTag, "stick {}", pressed ? "pressed" : "released");
        }
    }
    const int remote_channel = sent != 0 ? sent : said;
    if (_attached || _paused) {
        // On an app's WiFi, or on the router for a moment: its channel, nothing to look for or follow
    } else if (remote_channel != 0) {
        const bool follow = remote_channel != _channel && (!_followed || nowMs - _followed_ms >= kFollowEveryMs);
        if (_hopping || follow) {
            // Heard it (maybe a few channels off): on its channel from now on
            _hopping     = false;
            _probing     = false;
            _followed    = true;
            _followed_ms = nowMs;
            switch_channel(remote_channel);
            mclog::tagInfo(kTag, "remote on channel {}", remote_channel);
        }
        if (_channel == remote_channel && remote_channel != _saved_channel) {
            // Looked at first next time; the apps the remote opens on the next boot listen only there
            Settings settings(kNvsNamespace, true);
            settings.SetInt(kNvsChannel, remote_channel);
            _saved_channel = remote_channel;
        }
    } else if (_hopping) {
        // Looking for it: on the saved channel nearly all the time (a lone stick press is just two packets, they
        // have to be heard there), a look at one other channel in turn every kProbeEveryMs
        const uint32_t since = nowMs - _probe_ms;
        if (!_probing && since >= kProbeEveryMs) {
            _probe_channel = _probe_channel % 13 + 1;
            if (_probe_channel == _saved_channel) {
                _probe_channel = _probe_channel % 13 + 1;
            }
            _probing  = true;
            _probe_ms = nowMs;
            switch_channel(_probe_channel);
        } else if (_probing && since >= kProbeMs) {
            _probing  = false;
            _probe_ms = nowMs;
            switch_channel(_saved_channel);
        }
    }
    return packets;
}

int RemoteLink::receiverId()
{
    int id = _receiver_id.load();
    if (id < 0) {
        Settings settings(kNvsNamespace, false);
        id = settings.GetInt(kNvsId, kDefaultReceiverId);
        id = id >= 1 && id <= 255 ? id : kDefaultReceiverId;
        _receiver_id.store(id);
    }
    return id;
}

void RemoteLink::setReceiverId(int id)
{
    if (id < 1 || id > 255 || id == receiverId()) {
        return;
    }
    Settings settings(kNvsNamespace, true);
    settings.SetInt(kNvsId, id);
    _receiver_id.store(id);
}

bool RemoteLink::takeHomePress(uint32_t nowMs)
{
    bool pressed = false;
    bool copy    = false;
    int sent_on  = 0;
    if (!_listening) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(_mutex);
        // Signed: the press may have come in after the caller read the clock
        pressed       = _home_pressed && static_cast<int32_t>(nowMs - _home_ms) <= static_cast<int32_t>(kHomeFreshMs);
        copy          = _home_copy;
        sent_on       = _home_sent_on;
        _home_pressed = false;
    }
    // Which way it came (on an app's WiFi: the copy, or a packet heard a channel or few off, close by)
    if (pressed && copy) {
        mclog::tagInfo(kTag, "M5 button: home (its copy on every channel)");
    } else if (pressed) {
        mclog::tagInfo(kTag, "M5 button: home (sent on {})", sent_on);
    }
    return pressed;
}

bool RemoteLink::usedSincePowerOn()
{
    return __atomic_load_n(&s_used, __ATOMIC_RELAXED) == kUsedMark;
}

void RemoteLink::on_data(const std::vector<uint8_t>& data)
{
    // ESP-NOW task, its stack in PSRAM (patches/esp-now.patch): no NVS or flash here (receiverId() reads NVS: the
    // atomic copy instead). Only what parses as the remote's packet
    auto packet = parsePacket(data.data(), data.size());
    if (!packet) {
        return;
    }
    // The channel it was sent on (the main loop may have switched channels since)
    packet->sentOn = GetHAL().getEspNowRxChannel();
    const uint32_t now = GetHAL().millis();
    // -1 only before start() read it, and then nothing is heard: the default
    const int id = _receiver_id.load();
    std::lock_guard<std::mutex> lock(_mutex);
    const bool copy = packet->hasButtons && (packet->buttons & kButtonEverywhere) != 0;
    if (is_ours(*packet, id < 0 ? kDefaultReceiverId : id)) {
        // StackPet's remote (the buttons byte): in use from now on, until the robot is powered off
        if (packet->hasButtons) {
            __atomic_store_n(&s_used, kUsedMark, __ATOMIC_RELAXED);
        }
        // The M5 button, pressed: noted here for whichever app is open, the ones that take no packets too. By its
        // count: one press however many copies come, and whether its release was heard or not (on an app's WiFi
        // only the copies are). Without the count, by the button going down
        const bool down  = packet->hasButtons && (packet->buttons & kButtonHome) != 0;
        const bool press = packet->homeCount >= 0 ? down && packet->homeCount != _home_count : down && !_home_down;
        if (press) {
            _home_pressed = true;
            _home_copy    = copy;
            _home_sent_on = packet->sentOn;
            _home_ms      = now;
        }
        _home_down = down;
        if (packet->homeCount >= 0) {
            _home_count = packet->homeCount;
        }
    }
    if (copy) {
        return;  // the M5 press's copy says nothing of where the remote is, nor anything new: not handed out
    }
    if (_queue.size() >= kQueueLimit) {
        _queue.erase(_queue.begin());
    }
    _queue.push_back(*packet);
}

void RemoteLink::setDirectReceiver(bool direct)
{
    _direct_receiver = direct;
}

bool RemoteLink::takeDirectReceiver()
{
    const bool direct = _direct_receiver;
    _direct_receiver  = false;
    return direct;
}

}  // namespace stackpet::remote
