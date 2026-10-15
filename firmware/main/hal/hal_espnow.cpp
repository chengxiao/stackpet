/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "board/hal_bridge.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mooncake_log.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_err.h>
#include <esp_system.h>
#include <esp_event.h>
#include <lwip/err.h>
#include <lwip/sys.h>
#include <time.h>
#include <sys/time.h>
#include <esp_sntp.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <espnow.h>
#include <espnow_storage.h>
#include <espnow_utils.h>
#include <esp_check.h>
#include <esp_netif_net_stack.h>
#include <lwip/netif.h>
#include <lwip/etharp.h>
#include <lwip/ip4_addr.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>

// StackPet: patches/esp-now.patch puts the espnow task's 4 KB stack in PSRAM (the AI agent's internal RAM is tight);
// fetch_repos.py only says "skipped" when it does not apply
#ifndef ESPNOW_STACKPET_TASK_STACK_IN_PSRAM
#warning "patches/esp-now.patch is not applied to components/esp-now: the espnow task's stack is in internal RAM again"
#endif

static const std::string_view _tag = "HAL-EspNow";

static EventGroupHandle_t s_wifi_event_group = NULL;
static const int WIFI_CONNECTED_BIT          = BIT0;
static const int WIFI_DISCONNECTED_BIT       = BIT1;
static const int WIFI_FAIL_BIT               = BIT2;
static const int WIFI_STARTED_BIT            = BIT3;

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    const char* TAG = "WiFi";

    // Wifi started
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_STARTED_BIT);
    }

    // Disconnected
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        // StackPet: why, while the screensaver joins the router (the WiFi driver's own lines are quieted to warnings)
        const auto* event = static_cast<const wifi_event_sta_disconnected_t*>(event_data);
        ESP_LOGI(TAG, "disconnected, reason %d, rssi %d", event->reason, event->rssi);
        xEventGroupSetBits(s_wifi_event_group, WIFI_DISCONNECTED_BIT);
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    }

    // Connected
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }

    // Diagnosis (temporary): every other WiFi event (a scan done, a beacon timeout, a home channel change ...)
    else if (event_base == WIFI_EVENT) {
        ESP_LOGI(TAG, "event %d", static_cast<int>(event_id));
    }
}

// StackPet: kept so stopEspNow() can take the WiFi down again (the launcher listens for the remote, apps that use
// WiFi themselves start it their own way)
static esp_netif_t* s_sta_netif                    = nullptr;
static esp_event_handler_instance_t s_wifi_handler = nullptr;
static esp_event_handler_instance_t s_ip_handler   = nullptr;
static bool s_espnow_started                       = false;
static std::atomic<bool> s_espnow_attached{false};  // attachEspNow(): on an app's WiFi (the board asks, other tasks)
static int s_espnow_channel                        = 0;
static int s_espnow_rx_channel                     = 0;  // the ESP-NOW task's: the packet being handed out
// The WiFi driver's own level while ESP-NOW is up: its info lines are two per channel switch ("ic_enable_sniffer" ...)
static esp_log_level_t s_wifi_log_level = ESP_LOG_INFO;
static esp_log_level_t s_espnow_log_level = ESP_LOG_INFO;  // the espnow component's own, while attached
// espnow_init() returns an error only when its event group or mutex cannot be made (internal RAM gone; its other
// failures abort, ESP_ERROR_CHECK). It keeps its config then and says OK the next time, making nothing: so not tried
// again this boot
static bool s_espnow_init_failed = false;
// The router joined for a moment (joinRouter(), the screensaver's fetch): its channel meanwhile. The router's channel
// and BSSID from last time: straight there the next time, no scan (a shorter time away from the remote)
static std::atomic<bool> s_router_joined{false};
static uint8_t s_router_channel  = 0;
// The interface's MTU while joined: the Mac answers in segments of at most this less 40 (the MSS this side offers).
// Its 1.4 KB answer in one frame mostly never came through (a router that just saw this station join again sends at
// rates it does not keep up with: the SYN-ACK and other small frames did), three small ones do
constexpr uint16_t kRouterMtu = 576;

// Quiet: the remote link switches channels with it while it looks for the remote. StackPet: a channel the driver
// refuses (a country code that ends before 13) is skipped, not an abort on every boot
static bool _set_channel(int channel)
{
    channel = std::clamp(channel, 1, 13);

    // 建议先开启混杂模式再设信道，确保射频频率被强制锁定
    esp_err_t err = esp_wifi_set_promiscuous(true);
    if (err == ESP_OK) {
        err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    }
    esp_wifi_set_promiscuous(false);
    if (err != ESP_OK) {
        mclog::tagWarn(_tag, "wifi channel {}: {}", channel, esp_err_to_name(err));
        // Wherever the radio is (the first set on start can fail too, the channel is not 0 then)
        uint8_t primary            = 0;
        wifi_second_chan_t second  = WIFI_SECOND_CHAN_NONE;
        if (esp_wifi_get_channel(&primary, &second) == ESP_OK && primary >= 1 && primary <= 14) {
            s_espnow_channel = primary;
        }
        return false;
    }
    s_espnow_channel = channel;
    return true;
}

static void _wifi_init(int channel = 1)
{
    mclog::tagInfo(_tag, "wifi init");

    // ESP_ERROR_CHECK(nvs_flash_init());
    // StackPet: the TCP/IP stack before the STA interface is made on it: the screensaver joins the router for a moment
    // (joinRouter()), its first packet would find no stack and assert ("Invalid mbox"). Once a boot (it stays: the
    // apps that start the WiFi call it again, it is up already then)
    ESP_ERROR_CHECK(esp_netif_init());
    // StackPet: the default loop stays when stopEspNow() took the WiFi down, so it can be there already
    const esp_err_t loop = esp_event_loop_create_default();
    if (loop != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(loop);
    }
    esp_netif_t* sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);
    s_sta_netif = sta_netif;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    // StackPet: no A-MPDU reception on this WiFi (ESP-NOW, and the screensaver's few-second fetches on the router):
    // the router kept block-ack state from the last join a minute before, its aggregated answers never came through
    cfg.ampdu_rx_enable = 0;
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    if (!s_wifi_event_group) {
        s_wifi_event_group = xEventGroupCreate();
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr,
                                                        &s_wifi_handler));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr,
                                                        &s_ip_handler));

    ESP_ERROR_CHECK(esp_wifi_start());

    mclog::tagInfo(_tag, "wifi channel set to {}", std::clamp(channel, 1, 13));
    _set_channel(channel);
}

// StackPet: everything _wifi_init() set up but the default event loop (others keep handlers on it), so an app can
// start the WiFi its own way afterwards
static void _wifi_deinit()
{
    esp_wifi_stop();
    esp_wifi_deinit();
    if (s_wifi_handler != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler);
        s_wifi_handler = nullptr;
    }
    if (s_ip_handler != nullptr) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_handler);
        s_ip_handler = nullptr;
    }
    if (s_sta_netif != nullptr) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = nullptr;
    }
    s_espnow_channel = 0;
}

// StackPet: runs in the espnow component's task, whose stack is in PSRAM (patches/esp-now.patch): nothing here or in
// the onEspNowData handlers (RemoteLink::on_data) may touch NVS or the flash
static esp_err_t _handle_espnow_received(uint8_t* src_addr, void* data, size_t size, wifi_pkt_rx_ctrl_t* rx_ctrl)
{
    const char* TAG = "EspNow";

    ESP_PARAM_CHECK(src_addr);
    ESP_PARAM_CHECK(data);
    ESP_PARAM_CHECK(size);
    ESP_PARAM_CHECK(rx_ctrl);

    static uint32_t count = 0;

    // StackPet: debug, the remote sends every ~20 ms while it is used (the launcher and the RADAR listen too)
    ESP_LOGD(TAG, "espnow_recv, <%" PRIu32 "> [" MACSTR "][%d][%d][%u]: %.*s", count++, MAC2STR(src_addr),
             rx_ctrl->channel, rx_ctrl->rssi, size, size, "~");

    std::vector<uint8_t> received_data((uint8_t*)data, (uint8_t*)data + size);
    s_espnow_rx_channel = rx_ctrl->channel;  // StackPet: the channel it was sent on (from the frame header)
    GetHAL().onEspNowData.emit(received_data);

    return ESP_OK;
}

// onAppWifi: on the WiFi an app started (attachEspNow()). The receive callback runs in the WiFi task and waits for room
// in the component's queue up to send_max_timeout (3 s by default): there that would hold up the app's own traffic,
// so no waiting (a packet that finds the queue full is dropped; the M5 press comes three rounds over). And a smaller
// queue: mostly the M5 press's copies come there (and the remote's stream heard a few channels off, close by). Nothing
// is sent there (send_max_timeout also bounds sending)
static esp_err_t _espnow_init(bool onAppWifi)
{
    if (s_espnow_init_failed) {
        return ESP_ERR_INVALID_STATE;
    }
    espnow_config_t espnow_config = ESPNOW_INIT_CONFIG_DEFAULT();
    if (onAppWifi) {
        espnow_config.qsize            = 8;
        espnow_config.send_max_timeout = 0;
    }

    // 2. 修改关键参数以兼容 Arduino
    espnow_config.forward_enable         = false;  // 关闭转发（多跳），Arduino 无法解析带转发头的包
    espnow_config.forward_switch_channel = false;  // 关闭自动切信道
    espnow_config.send_retry_num         = 5;      // 失败重试次数（可按需调，建议5-10）

    // 3. 修改接收使能开关
    espnow_config.receive_enable.forward = false;  // 关闭转发包接收
    espnow_config.receive_enable.data    = true;   // 必须开启这个，才能接收 Arduino 发来的普通数据包

    const esp_err_t err = espnow_init(&espnow_config);
    if (err != ESP_OK) {
        s_espnow_init_failed = true;
        return err;
    }
    espnow_set_config_for_data_type(ESPNOW_DATA_TYPE_DATA, true, _handle_espnow_received);
    s_espnow_started = true;
    return ESP_OK;
}

void Hal::startEspNow(int channel)
{
    if (s_espnow_started) {
        // StackPet: already listening (the launcher does, for the remote): only the channel, if it differs (on an
        // app's WiFi it is the router's: left alone)
        if (!s_espnow_attached && std::clamp(channel, 1, 13) != s_espnow_channel) {
            mclog::tagInfo(_tag, "EspNow already started, wifi channel set to {}", std::clamp(channel, 1, 13));
            _set_channel(channel);
        }
        return;
    }
    if (s_espnow_init_failed) {
        return;  // not this boot (said when it failed)
    }
    mclog::tagInfo(_tag, "start EspNow on channel {}", channel);

    _wifi_init(channel);
    const esp_err_t err = _espnow_init(false);
    if (err != ESP_OK) {
        // The WiFi given back, for the apps that start their own (the link hears nothing this boot)
        mclog::tagWarn(_tag, "EspNow init failed: {}, the WiFi off again", esp_err_to_name(err));
        _wifi_deinit();
        return;
    }
    // Quiet while it switches channels looking for the remote (the driver logs two lines a switch)
    s_wifi_log_level = esp_log_level_get("wifi");
    esp_log_level_set("wifi", ESP_LOG_WARN);

    mclog::tagInfo(_tag, "factory mac: {}", getFactoryMacString());
}

bool Hal::attachEspNow()
{
    if (s_espnow_started) {
        return true;
    }
    if (s_espnow_init_failed) {
        return false;  // not this boot (said when it failed)
    }
    // The app's WiFi has to be connected to its router: espnow_init() then stays on that channel (it never switches)
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        mclog::tagWarn(_tag, "no WiFi connected: no EspNow on it");
        return false;
    }
    const esp_err_t err = _espnow_init(true);
    if (err != ESP_OK) {
        mclog::tagWarn(_tag, "EspNow on the app's WiFi failed: {}", esp_err_to_name(err));
        return false;
    }
    s_espnow_attached = true;
    s_espnow_channel  = ap.primary;
    // A packet dropped on a full queue logs a warning from the WiFi task (the UART busy for ms each): errors only
    s_espnow_log_level = esp_log_level_get("espnow");
    esp_log_level_set("espnow", ESP_LOG_ERROR);
    // Asleep between the router's beacons the radio misses the remote: awake while it listens (the board keeps it so
    // when the AI agent asks for power save, hal_bridge::espnow_keeps_radio_awake())
    esp_wifi_set_ps(WIFI_PS_NONE);
    mclog::tagInfo(_tag, "EspNow on the app's WiFi, channel {}, power save off", s_espnow_channel);
    return true;
}

bool hal_bridge::espnow_keeps_radio_awake()
{
    return s_espnow_attached;
}

void Hal::setEspNowChannel(int channel)
{
    if (s_espnow_started && !s_espnow_attached && !s_router_joined) {
        _set_channel(channel);  // attached, or the router joined: its channel
    }
}

namespace {
void air_start(const uint8_t* bssid, uint32_t nowMs);  // diagnosis (temporary), below
}

bool Hal::joinRouter(const std::string& ssid, const std::string& password, uint32_t timeoutMs,
                     const std::function<bool()>& aborted)
{
    if (!s_espnow_started || s_espnow_attached || s_router_joined) {
        return false;
    }
    s_router_joined = true;  // first: from here on setEspNowChannel() leaves the radio alone

    wifi_config_t cfg = {};
    // Zeroed: shorter ones end in NUL, a 32-byte SSID or 64-byte password fills its field (as the driver takes them)
    std::memcpy(cfg.sta.ssid, ssid.data(), std::min(ssid.size(), sizeof(cfg.sta.ssid)));
    std::memcpy(cfg.sta.password, password.data(), std::min(password.size(), sizeof(cfg.sta.password)));
    cfg.sta.scan_method = WIFI_FAST_SCAN;
    // The channel it was on, looked at first; not one access point (a mesh's other one often refused a join to it)
    cfg.sta.channel = s_router_channel;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_DISCONNECTED_BIT | WIFI_FAIL_BIT);
    // The config in RAM only: by default the driver writes it to NVS, here once a minute while the screensaver shows
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    // 20 MHz, as the channel switches set the radio (WIFI_SECOND_CHAN_NONE): told HT40, the router sent its large frames
    // at rates this side was not listening for (small packets came through, the Mac's 1.4 KB answer never did)
    esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
    // Awake while joined: in modem sleep the Mac's answers (its ARP reply, the SYN-ACK) wait for the router's beacons;
    // the first fetches took 1.6 s to connect, or timed out. Off the router it does not matter (ESP-NOW listens)
    esp_wifi_set_ps(WIFI_PS_NONE);
    if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK || esp_wifi_connect() != ESP_OK) {
        return false;
    }
    // Connected and an IP (DHCP on the launcher's own STA interface), or the driver gave up (a disconnect: it does
    // not retry on its own here)
    const uint32_t start = millis();
    while (!aborted() && millis() - start < timeoutMs) {
        const EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE,
                                                     pdFALSE, pdMS_TO_TICKS(100));
        if (bits & WIFI_CONNECTED_BIT) {
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                s_router_channel = ap.primary;
                int8_t power = 0;
                esp_wifi_get_max_tx_power(&power);
                wifi_phy_mode_t mode = WIFI_PHY_MODE_LR;
                const bool have_mode = esp_wifi_sta_get_negotiated_phymode(&mode) == ESP_OK;
                wifi_ps_type_t ps    = WIFI_PS_MAX_MODEM;
                esp_wifi_get_ps(&ps);
                ESP_LOGI("WiFi", "power save %d", static_cast<int>(ps));
                ESP_LOGI("WiFi",
                         "joined " MACSTR " on %d%s, rssi %d, tx power %d (0.25 dBm), phy %s%s%s%s (negotiated %d), "
                         "auth %d, ciphers %d/%d, bw %d, bss color %d, country %.2s",
                         MAC2STR(ap.bssid), ap.primary,
                         ap.second == WIFI_SECOND_CHAN_ABOVE   ? "+"
                         : ap.second == WIFI_SECOND_CHAN_BELOW ? "-"
                                                               : "",
                         ap.rssi, power, ap.phy_11b ? "b" : "", ap.phy_11g ? "g" : "", ap.phy_11n ? "n" : "",
                         ap.phy_11ax ? "ax" : "", have_mode ? static_cast<int>(mode) : -1, ap.authmode,
                         ap.pairwise_cipher, ap.group_cipher, ap.bandwidth, ap.he_ap.bss_color, ap.country.cc);
                air_start(ap.bssid, millis());
            }
            // In the TCP/IP task, where the interface is used
            esp_netif_tcpip_exec(
                [](void* ctx) -> esp_err_t {
                    auto* lwip = static_cast<struct netif*>(esp_netif_get_netif_impl(static_cast<esp_netif_t*>(ctx)));
                    if (lwip != nullptr) {
                        lwip->mtu = kRouterMtu;
                    }
                    return ESP_OK;
                },
                s_sta_netif);
            return true;
        }
        if (bits & WIFI_FAIL_BIT) {
            break;
        }
    }
    s_router_channel = 0;  // a full scan next time: it may have moved
    return false;
}

// Diagnosis (temporary): the air while joined, heard in promiscuous mode. The callback (the WiFi task) only counts,
// into PSRAM (internal RAM is tight for the AI agent); the report after promiscuous mode is off again
namespace {
constexpr int kAirBins  = 64;  // 250 ms each: 16 s
constexpr int kAirBinMs = 250;
constexpr int kAirGaps  = 8;
constexpr int kAirToUs  = 24;
struct Air {
    int64_t start_us = 0;
    uint32_t start_ms = 0;
    uint8_t bssid[6]  = {};
    uint8_t self[6]   = {};
    uint32_t frames = 0, mgmt = 0, ctrl = 0, data = 0;
    uint32_t beacons        = 0;
    int64_t last_beacon_us  = 0;
    uint32_t max_gap_ms     = 0;
    int gaps                = 0;
    uint32_t gap_end_ms[kAirGaps] = {};
    uint32_t gap_len_ms[kAirGaps] = {};
    uint32_t nav_long      = 0;
    uint32_t nav_max_us    = 0;
    uint8_t nav_max_from[6] = {};
    uint8_t nav_max_fc     = 0;
    int nf_min = 127, nf_max = -128;
    int32_t nf_sum = 0;
    uint32_t nf_n  = 0;
    int to_us      = 0;
    uint32_t to_us_ms[kAirToUs]  = {};
    uint16_t to_us_len[kAirToUs] = {};
    uint16_t to_us_seq[kAirToUs] = {};
    uint8_t to_us_fc1[kAirToUs]  = {};
    uint32_t actions[8] = {};  // to this station by category: spectrum, block ack, radio measurement, SA query, WNM,
                               // vendor, HT, other
    uint32_t deauth = 0;
    uint32_t quiet = 0, csa = 0;
    uint8_t wmm[16] = {};
    bool have_wmm   = false;
    uint16_t bin_frames[kAirBins]  = {};
    uint16_t bin_beacons[kAirBins] = {};
    uint16_t bin_acks[kAirBins]    = {};  // ACKs and block acks to this station: when its own frames got through
    uint16_t bin_to_us[kAirBins]   = {};
    uint32_t bin_nav_us[kAirBins]  = {};
};
Air* s_air = nullptr;
std::atomic<bool> s_air_on{false};

void air_rx(void* buf, wifi_promiscuous_pkt_type_t type)
{
    Air* a = s_air;
    if (a == nullptr || !s_air_on.load(std::memory_order_relaxed)) {
        return;
    }
    const auto* pkt   = static_cast<const wifi_promiscuous_pkt_t*>(buf);
    const uint8_t* p  = pkt->payload;
    const int len     = pkt->rx_ctrl.sig_len;  // with the FCS
    const int64_t now = esp_timer_get_time();
    const uint32_t ms = static_cast<uint32_t>((now - a->start_us) / 1000);
    const int bin     = static_cast<int>(ms / kAirBinMs);
    ++a->frames;
    if (bin < kAirBins) {
        ++a->bin_frames[bin];
    }
    const int nf = pkt->rx_ctrl.noise_floor;
    if (nf != 0) {
        a->nf_min = std::min(a->nf_min, nf);
        a->nf_max = std::max(a->nf_max, nf);
        a->nf_sum += nf;
        ++a->nf_n;
    }
    if (len < 10) {
        return;
    }
    const uint8_t fc0  = p[0];
    const uint8_t fc1  = p[1];
    const uint16_t dur = static_cast<uint16_t>(p[2] | (p[3] << 8));
    if ((dur & 0x8000) == 0) {
        if (bin < kAirBins) {
            a->bin_nav_us[bin] += dur;
        }
        if (dur > 3000) {
            ++a->nav_long;
            if (dur > a->nav_max_us) {
                a->nav_max_us = dur;
                a->nav_max_fc = fc0;
                if (len >= 16) {
                    std::memcpy(a->nav_max_from, p + 10, 6);
                } else {
                    std::memset(a->nav_max_from, 0, 6);
                }
            }
        }
    }
    const bool for_us = std::memcmp(p + 4, a->self, 6) == 0;
    const int subtype = (fc0 >> 4) & 0xF;
    if (type == WIFI_PKT_MGMT) {
        ++a->mgmt;
        if (len < 28) {
            return;
        }
        const bool from_ap = std::memcmp(p + 10, a->bssid, 6) == 0;
        if (subtype == 8 && from_ap) {
            ++a->beacons;
            if (bin < kAirBins) {
                ++a->bin_beacons[bin];
            }
            if (a->last_beacon_us != 0) {
                const uint32_t gap = static_cast<uint32_t>((now - a->last_beacon_us) / 1000);
                a->max_gap_ms      = std::max(a->max_gap_ms, gap);
                if (gap >= 300) {
                    if (a->gaps < kAirGaps) {
                        a->gap_end_ms[a->gaps] = ms;
                        a->gap_len_ms[a->gaps] = gap;
                    }
                    ++a->gaps;
                }
            }
            a->last_beacon_us = now;
            // The elements after the timestamp, the interval and the capabilities
            int i         = 36;
            const int end = len - 4;
            while (i + 2 <= end) {
                const int id = p[i];
                const int l  = p[i + 1];
                if (i + 2 + l > end) {
                    break;
                }
                if (id == 40) {
                    ++a->quiet;
                } else if (id == 37 || id == 60) {
                    ++a->csa;
                } else if (id == 221 && l >= 24 && !a->have_wmm && p[i + 2] == 0x00 && p[i + 3] == 0x50 &&
                           p[i + 4] == 0xf2 && p[i + 5] == 2 && p[i + 6] == 1) {
                    std::memcpy(a->wmm, p + i + 2 + 8, 16);  // the four AC records
                    a->have_wmm = true;
                }
                i += 2 + l;
            }
        } else if (for_us && subtype == 13) {
            const int cat = p[24];
            const int idx = cat == 0 ? 0 : cat == 3 ? 1 : cat == 5 ? 2 : cat == 8 ? 3 : cat == 10 ? 4 : cat == 127 ? 5
                                                                                   : cat == 7  ? 6
                                                                                               : 7;
            ++a->actions[idx];
        } else if (for_us && (subtype == 10 || subtype == 12)) {
            ++a->deauth;
        }
    } else if (type == WIFI_PKT_CTRL) {
        ++a->ctrl;
        if (for_us && (subtype == 13 || subtype == 9) && bin < kAirBins) {
            ++a->bin_acks[bin];
        }
    } else if (type == WIFI_PKT_DATA) {
        ++a->data;
        if (for_us && len >= 24) {
            if (bin < kAirBins) {
                ++a->bin_to_us[bin];
            }
            if (a->to_us < kAirToUs) {
                a->to_us_ms[a->to_us]  = ms;
                a->to_us_len[a->to_us] = static_cast<uint16_t>(len);
                a->to_us_seq[a->to_us] = static_cast<uint16_t>((p[22] | (p[23] << 8)) >> 4);
                a->to_us_fc1[a->to_us] = fc1;
            }
            ++a->to_us;
        }
    }
}

void air_start(const uint8_t* bssid, uint32_t nowMs)
{
    if (s_air == nullptr) {
        s_air = static_cast<Air*>(heap_caps_calloc(1, sizeof(Air), MALLOC_CAP_SPIRAM));
        if (s_air == nullptr) {
            return;
        }
    }
    *s_air          = Air{};
    s_air->start_us = esp_timer_get_time();
    s_air->start_ms = nowMs;
    std::memcpy(s_air->bssid, bssid, 6);
    esp_wifi_get_mac(WIFI_IF_STA, s_air->self);
    wifi_promiscuous_filter_t filter{};
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_CTRL | WIFI_PROMIS_FILTER_MASK_DATA;
    esp_wifi_set_promiscuous_filter(&filter);
    wifi_promiscuous_filter_t ctrl{};
    ctrl.filter_mask = WIFI_PROMIS_CTRL_FILTER_MASK_ALL;
    esp_wifi_set_promiscuous_ctrl_filter(&ctrl);
    esp_wifi_set_promiscuous_rx_cb(&air_rx);
    s_air_on = true;
    const esp_err_t err = esp_wifi_set_promiscuous(true);
    ESP_LOGI("WiFi", "air: listening (%s)", esp_err_to_name(err));
}

std::string air_bins(const char* name, const uint16_t* values)
{
    std::string s = name;
    char buf[8];
    for (int i = 0; i < kAirBins; ++i) {
        std::snprintf(buf, sizeof(buf), " %u", values[i]);
        s += buf;
    }
    return s;
}

void air_report()
{
    if (s_air == nullptr || !s_air_on) {
        return;
    }
    s_air_on = false;
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    wifi_promiscuous_filter_t filter{};
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_ALL;
    esp_wifi_set_promiscuous_filter(&filter);
    vTaskDelay(pdMS_TO_TICKS(20));  // a callback still running finishes
    const Air& a      = *s_air;
    const uint32_t ms = static_cast<uint32_t>((esp_timer_get_time() - a.start_us) / 1000);
    std::string gaps;
    char buf[160];
    for (int i = 0; i < std::min(a.gaps, kAirGaps); ++i) {
        std::snprintf(buf, sizeof(buf), " %u/%u", static_cast<unsigned>(a.gap_end_ms[i]),
                      static_cast<unsigned>(a.gap_len_ms[i]));
        gaps += buf;
    }
    ESP_LOGI("WiFi",
             "air from boot ms %u for %u ms: %u frames (mgmt %u, ctrl %u, data %u); our beacons %u, longest gap %u "
             "ms, %d gaps of 300 ms or more (end/length):%s",
             static_cast<unsigned>(a.start_ms), static_cast<unsigned>(ms), static_cast<unsigned>(a.frames),
             static_cast<unsigned>(a.mgmt), static_cast<unsigned>(a.ctrl), static_cast<unsigned>(a.data),
             static_cast<unsigned>(a.beacons), static_cast<unsigned>(a.max_gap_ms), a.gaps, gaps.c_str());
    ESP_LOGI("WiFi",
             "air: noise floor %d / %d / %d; NAV over 3 ms %u times, the longest %u us (fc %02x from " MACSTR
             "); deauth %u, quiet %u, csa %u; actions to us (spectrum, ba, rm, sa, wnm, vendor, ht, other) %u %u %u "
             "%u %u %u %u %u",
             a.nf_min, a.nf_n > 0 ? static_cast<int>(a.nf_sum / static_cast<int32_t>(a.nf_n)) : 0, a.nf_max,
             static_cast<unsigned>(a.nav_long), static_cast<unsigned>(a.nav_max_us), a.nav_max_fc,
             MAC2STR(a.nav_max_from), static_cast<unsigned>(a.deauth), static_cast<unsigned>(a.quiet),
             static_cast<unsigned>(a.csa), static_cast<unsigned>(a.actions[0]), static_cast<unsigned>(a.actions[1]),
             static_cast<unsigned>(a.actions[2]), static_cast<unsigned>(a.actions[3]),
             static_cast<unsigned>(a.actions[4]), static_cast<unsigned>(a.actions[5]),
             static_cast<unsigned>(a.actions[6]), static_cast<unsigned>(a.actions[7]));
    if (a.have_wmm) {
        ESP_LOGI("WiFi", "air: WMM (aci/aifsn ecw txop) BE %02x %02x %02x%02x BK %02x %02x %02x%02x VI %02x %02x "
                         "%02x%02x VO %02x %02x %02x%02x",
                 a.wmm[0], a.wmm[1], a.wmm[3], a.wmm[2], a.wmm[4], a.wmm[5], a.wmm[7], a.wmm[6], a.wmm[8], a.wmm[9],
                 a.wmm[11], a.wmm[10], a.wmm[12], a.wmm[13], a.wmm[15], a.wmm[14]);
    }
    std::string to_us;
    for (int i = 0; i < std::min(a.to_us, kAirToUs); ++i) {
        std::snprintf(buf, sizeof(buf), " %u:%u:%u:%02x", static_cast<unsigned>(a.to_us_ms[i]), a.to_us_len[i],
                      a.to_us_seq[i], a.to_us_fc1[i]);
        to_us += buf;
    }
    ESP_LOGI("WiFi", "air: data to us %d (ms:len:seq:fc1):%s", a.to_us, to_us.c_str());
    ESP_LOGI("WiFi", "%s", air_bins("air bins frames", a.bin_frames).c_str());
    ESP_LOGI("WiFi", "%s", air_bins("air bins beacons", a.bin_beacons).c_str());
    ESP_LOGI("WiFi", "%s", air_bins("air bins acks to us", a.bin_acks).c_str());
    ESP_LOGI("WiFi", "%s", air_bins("air bins data to us", a.bin_to_us).c_str());
    uint16_t nav_ms[kAirBins];
    for (int i = 0; i < kAirBins; ++i) {
        nav_ms[i] = static_cast<uint16_t>(std::min<uint32_t>(a.bin_nav_us[i] / 1000, 65535));
    }
    ESP_LOGI("WiFi", "%s", air_bins("air bins NAV ms", nav_ms).c_str());
}
}  // namespace

void Hal::routerStatsDump()
{
    air_report();
    const esp_log_level_t level = esp_log_level_get("wifi");
    esp_log_level_set("wifi", ESP_LOG_INFO);
    esp_wifi_statis_dump(WIFI_STATIS_ALL);
    esp_log_level_set("wifi", level);
}

std::string Hal::routerArpFor(const std::string& ipv4)
{
    struct Look {
        esp_netif_t* netif;
        ip4_addr_t ip;
        bool found;
        uint8_t mac[6];
    } look{s_sta_netif, {}, false, {}};
    if (s_sta_netif == nullptr || !ip4addr_aton(ipv4.c_str(), &look.ip)) {
        return "none";
    }
    esp_netif_tcpip_exec(
        [](void* ctx) -> esp_err_t {
            auto* l    = static_cast<Look*>(ctx);
            auto* lwip = static_cast<struct netif*>(esp_netif_get_netif_impl(l->netif));
            struct eth_addr* eth  = nullptr;
            const ip4_addr_t* got = nullptr;
            l->found = lwip != nullptr && etharp_find_addr(lwip, &l->ip, &eth, &got) >= 0 && eth != nullptr;
            if (l->found) {
                std::memcpy(l->mac, eth->addr, sizeof(l->mac));
            }
            return ESP_OK;
        },
        &look);
    if (!look.found) {
        return "none";
    }
    char buf[18];
    std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", look.mac[0], look.mac[1], look.mac[2],
                  look.mac[3], look.mac[4], look.mac[5]);
    return buf;
}

void Hal::leaveRouter(int channel)
{
    if (!s_router_joined) {
        return;
    }
    xEventGroupClearBits(s_wifi_event_group, WIFI_DISCONNECTED_BIT);
    if (esp_wifi_disconnect() == ESP_OK) {
        // Off the router before the channel changes (also when it was still connecting: that ends with a disconnect)
        xEventGroupWaitBits(s_wifi_event_group, WIFI_DISCONNECTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));
    }
    s_router_joined = false;
    _set_channel(channel);
}

int Hal::getEspNowChannel()
{
    return s_espnow_started ? s_espnow_channel : 0;
}

int Hal::getEspNowRxChannel()
{
    return s_espnow_rx_channel;
}

// StackPet: the espnow component's task ends a moment after espnow_deinit(), on its own, deleting the queue it finds
// then: an espnow_init() right after would lose its new queue to it (upstream). Nothing starts ESP-NOW again that soon
// (the soonest is ESPNOW.REMOTE's role page, someone has to tap it)
void Hal::stopEspNow()
{
    if (!s_espnow_started) {
        return;
    }
    mclog::tagInfo(_tag, "stop EspNow");
    if (s_espnow_attached) {
        // On an app's WiFi: that stays
        espnow_deinit();
        s_espnow_attached = false;
        s_espnow_started  = false;
        s_espnow_channel  = 0;
        esp_log_level_set("espnow", s_espnow_log_level);
        return;
    }
    espnow_deinit();
    _wifi_deinit();
    s_espnow_started = false;
    esp_log_level_set("wifi", s_wifi_log_level);
}

bool Hal::espNowSend(const std::vector<uint8_t>& data, const uint8_t* destAddr)
{
    mclog::tagInfo(_tag, "send data with size: {}", data.size());

    espnow_frame_head_t frame_head = ESPNOW_FRAME_CONFIG_DEFAULT();
    esp_err_t ret                  = ESP_FAIL;

    if (destAddr == nullptr) {
        ret = espnow_send(ESPNOW_DATA_TYPE_DATA, ESPNOW_ADDR_BROADCAST, data.data(), data.size(), &frame_head,
                          portMAX_DELAY);
    } else {
        ret = espnow_send(ESPNOW_DATA_TYPE_DATA, destAddr, data.data(), data.size(), &frame_head, portMAX_DELAY);
    }

    if (ret != ESP_OK) {
        mclog::tagError(_tag, "send failed: {}", esp_err_to_name(ret));
        return false;
    }
    return true;
}

#include <driver/gpio.h>

void Hal::setLaserEnabled(bool enabled)
{
    static bool laser_enabled = false;
    static bool is_inited     = false;

    if (laser_enabled == enabled) {
        return;
    }

    const gpio_num_t laser_pin = GPIO_NUM_2;

    if (!is_inited) {
        gpio_reset_pin(laser_pin);
        gpio_set_direction(laser_pin, GPIO_MODE_OUTPUT);
        gpio_set_pull_mode(laser_pin, GPIO_PULLUP_ONLY);
        is_inited = true;
    }

    mclog::tagInfo(_tag, "set laser {}", enabled ? "enabled" : "disabled");

    if (enabled) {
        gpio_set_level(laser_pin, 1);
    } else {
        gpio_set_level(laser_pin, 0);
    }
    laser_enabled = enabled;
}
