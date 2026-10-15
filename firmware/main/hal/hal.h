/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <memory>
#include <cstdint>
#include <string>
#include <lvgl.h>
#include <functional>
#include <smooth_ui_toolkit.hpp>
#include <uitk/short_namespace.hpp>
#include <smooth_lvgl.hpp>
#include <array>
#include <lvgl_image.h>
#include <string_view>

/**
 * @brief
 *
 */
enum class HeadPetGesture { None, Press, Release, SwipeForward, SwipeBackward };

/**
 * @brief
 *
 */
enum class WsSignalSource {
    Local = 0,
    Remote,
};

/**
 * @brief
 *
 */
struct WsTextMessage_t {
    std::string name;
    std::string content;
};

/**
 * @brief
 *
 */
enum class ImuMotionEvent {
    None = 0,
    Shake,
    PickUp,
};

/**
 * @brief
 *
 */
enum class AppConfigEvent {
    None = 0,
    AppConnected,
    AppDisconnected,
    TryWifiConnect,
    WifiConnectFailed,
    WifiConnected,
};

/**
 * @brief
 *
 */
enum class CommonLogLevel {
    Info = 0,
    Warning,
    Error,
};

/**
 * @brief
 *
 */
namespace app_center {

struct AppInfo_t {
    std::string name;
    std::string iconUrl;
    std::string description;
    std::string firmwareUrl;
};

using AppInfoList_t = std::vector<AppInfo_t>;

};  // namespace app_center

/**
 * @brief
 *
 */
enum class WifiStatus {
    None = 0,
    Low,
    Medium,
    High,
};

/**
 * @brief
 *
 */
struct UserAccountInfo_t {
    std::string username;
    std::string deviceName;
};

/**
 * @brief
 *
 */
struct XiaozhiConfig_t {
    uint32_t idleShutdownTimeSeconds = 600;
    bool allowShutdownWhenCharging   = false;
    uint8_t idleRandomMovementLevel  = 2;
    bool startAiAgentOnBoot          = false;
};

/**
 * @brief
 *
 */
enum class MicTestStatus {
    Starting = 0,
    Recording,
    Playing,
    Done,
    Failed,
};

/**
 * @brief
 *
 */
class BootLogo {
public:
    BootLogo()
    {
        _panel = std::make_unique<uitk::lvgl_cpp::Container>(lv_screen_active());
        _panel->setSize(320, 240);
        _panel->setAlign(LV_ALIGN_CENTER);
        _panel->setBorderWidth(0);
        _panel->setBgOpa(0);
        _panel->setPaddingAll(0);

        _label_logo = std::make_unique<uitk::lvgl_cpp::Label>(_panel->get());
        _label_logo->setTextFont(&lv_font_montserrat_24);
        _label_logo->setTextColor(lv_color_hex(0xFFFFFF));
        _label_logo->align(LV_ALIGN_CENTER, 0, -14);
        _label_logo->setText("STACKCHAN");

        _label_msg = std::make_unique<uitk::lvgl_cpp::Label>(_panel->get());
        _label_msg->setTextFont(&lv_font_montserrat_16);
        _label_msg->setTextColor(lv_color_hex(0xBFBFBF));
        _label_msg->align(LV_ALIGN_CENTER, 0, 14);
        _label_msg->setText("Starting up ...");

        _label_version = std::make_unique<uitk::lvgl_cpp::Label>(_panel->get());
        _label_version->setTextFont(&lv_font_montserrat_14);
        _label_version->setTextColor(lv_color_hex(0x8B8B8B));
        _label_version->align(LV_ALIGN_BOTTOM_RIGHT, -7, -6);
        _label_version->setText("V" FIRMWARE_VERSION);
    }

private:
    std::unique_ptr<uitk::lvgl_cpp::Container> _panel;
    std::unique_ptr<uitk::lvgl_cpp::Label> _label_logo;
    std::unique_ptr<uitk::lvgl_cpp::Label> _label_msg;
    std::unique_ptr<uitk::lvgl_cpp::Label> _label_version;
};

/**
 * @brief
 *
 */
class Hal {
public:
    void init();

    /* --------------------------------- System --------------------------------- */
    void delay(std::uint32_t ms);
    std::uint32_t millis();
    void feedTheDog();
    std::array<uint8_t, 6> getFactoryMac();
    std::string getFactoryMacString(std::string divider = "");
    void reboot();
    void updateHeapStatusLog();
    uint8_t getBatteryLevel();
    bool isBatteryCharging();
    bool hasExternalPower();  // StackPet: on a cable (charging or full): no power saving then
    void factoryReset();

    /* --------------------------------- Display -------------------------------- */
    lv_indev_t* lvTouchpad = nullptr;
    std::unique_ptr<BootLogo> bootLogo;
    void lvglLock();
    void lvglUnlock();
    void setBackLightBrightness(uint8_t brightness, bool permanent = false);
    uint8_t getBackLightBrightness();

    /* --------------------------------- Xiaozhi -------------------------------- */
    void requestXiaozhiStart()
    {
        _xiaozhi_start_requested = true;
    }
    bool isXiaozhiStartRequested()
    {
        return _xiaozhi_start_requested;
    }
    void startXiaozhi();
    XiaozhiConfig_t getXiaozhiConfig();
    void setXiaozhiConfig(XiaozhiConfig_t config);

    /* ----------------------------------- BLE ---------------------------------- */
    uitk::Signal<const char*> onBleMotionData;
    uitk::Signal<const char*> onBleAvatarData;
    uitk::Signal<const char*> onBleConfigData;
    uitk::Signal<const char*> onBleRgbData;
    uitk::Signal<AppConfigEvent> onAppConfigEvent;

    void startBleServer();
    bool isBleConnected();
    void startAppConfigServer();
    bool isAppConfiged();
    void resetAppConfiged();

    /* --------------------------------- HeadPet -------------------------------- */
    uitk::Signal<HeadPetGesture> onHeadPetGesture;

    /* ----------------------------------- RGB ---------------------------------- */
    void setRgbColor(uint8_t index, uint8_t r, uint8_t g, uint8_t b);
    void showRgbColor(uint8_t r, uint8_t g, uint8_t b);
    void refreshRgb();

    /* ---------------------------------- Power --------------------------------- */
    void setServoPowerEnabled(bool enabled);

    /* -------------------------------- Websocket ------------------------------- */
    uitk::Signal<std::string_view> onWsMotionData;
    uitk::Signal<std::string_view> onWsAvatarData;
    uitk::Signal<std::string> onWsCallRequest;
    uitk::Signal<bool> onWsCallResponse;
    uitk::Signal<WsSignalSource> onWsCallEnd;
    uitk::Signal<const WsTextMessage_t&> onWsTextMessage;
    uitk::Signal<bool> onWsVideoModeChange;
    uitk::Signal<std::shared_ptr<LvglImage>> onWsVideoFrame;
    uitk::Signal<std::string_view> onWsDanceData;
    uitk::Signal<CommonLogLevel, std::string_view> onWsLog;

    void startWebSocketAvatarService(std::function<void(std::string_view)> onStartLog);

    /* ----------------------------------- IMU ---------------------------------- */
    uitk::Signal<ImuMotionEvent> onImuMotionEvent;

    /* ---------------------------------- Time ---------------------------------- */
    void syncRtcTimeToSystem();
    void syncSystemTimeToRtc();
    void setTimezone(std::string_view tz);
    std::string getTimezone();

    /* --------------------------------- EspNow --------------------------------- */
    uitk::Signal<const std::vector<uint8_t>&> onEspNowData;
    void startEspNow(int channel);  // StackPet: when already started only the channel changes (attached: not even that)
    bool espNowSend(const std::vector<uint8_t>& data, const uint8_t* destAddr = nullptr);
    // StackPet: the launcher listens for the remote from boot on; apps that start WiFi their own way stop it first
    void stopEspNow();
    // StackPet: ESP-NOW on the WiFi an app started (connected to the router: its channel, no switching), the radio
    // kept awake (power save off) while it listens; false if the WiFi is not up, or ESP-NOW cannot start this boot
    // (its init failed once: internal RAM gone). stopEspNow() leaves that WiFi on
    bool attachEspNow();
    // StackPet: the launcher's WiFi (ESP-NOW, listening for the remote) onto the router for a moment: the screensaver
    // fetches Claude's usage from the Mac. The router's channel meanwhile (setEspNowChannel() does nothing). False if
    // ESP-NOW is not up the launcher's way, or no IP within timeoutMs, or aborted() said so; leaveRouter() either way
    // (back on `channel`). It waits: the fetch's own task, never the main loop
    bool joinRouter(const std::string& ssid, const std::string& password, uint32_t timeoutMs,
                    const std::function<bool()>& aborted);
    void leaveRouter(int channel);
    // StackPet (the fetch's log): the MAC the ARP table has for an IPv4 address ("12:34:56:78:9a:bc"), "none"
    std::string routerArpFor(const std::string& ipv4);
    void routerStatsDump();  // StackPet (diagnosis): the air heard while joined, the WiFi driver's counters to the log
    void setEspNowChannel(int channel);
    int getEspNowChannel();    // 0: not started
    // The channel the packet onEspNowData is handing out was sent on (its handlers ask): the espnow component puts its
    // frame header's channel into rx_ctrl->channel
    int getEspNowRxChannel();
    void setLaserEnabled(bool enabled);

    /* ------------------------------- Warm Reboot ------------------------------ */
    void requestWarmReboot(int appIndex);
    int getWarmRebootTarget();
    void clearWarmRebootRequest();

    /* --------------------------------- Network -------------------------------- */
    void startNetwork(std::function<void(std::string_view)> onLog);
    WifiStatus getWifiStatus();
    void startSntp();

    /* -------------------------------- App center ------------------------------- */
    app_center::AppInfoList_t fetchAppList();
    void launchApp(std::string_view url, std::function<void(int)> onProgress);

    /* --------------------------------- EzData --------------------------------- */
    void startEzDataService(std::function<void(std::string_view)> onStartLog);
    uitk::Signal<std::string_view> onEzdataPairCode;

    /* ------------------------------- User Acount ------------------------------ */
    UserAccountInfo_t getUserAccountInfo();
    bool updateAccountInfo(std::function<void(std::string_view)> onLog);
    bool unbindAccount(std::function<void(std::string_view)> onLog);

    /* ----------------------------------- OTA ---------------------------------- */
    bool updateFirmware(std::function<void(std::string_view)> onLog);

    /* ---------------------------------- Audio --------------------------------- */
    void setSpeakerVolume(uint8_t volume, bool permanent = false);
    uint8_t getSpeakerVolume();
    std::string startMicTest(std::function<void(MicTestStatus)> onStatusUpdate);
    void getMicWaveformFrame(std::vector<int16_t>& data);
    void clearupMicTest();

private:
    bool _xiaozhi_start_requested = false;

    void xiaozhi_board_init();
    void lvgl_init();
    void xiaozhi_mcp_init();
    void ble_init(bool useAltUuid);
    void servo_init();
    void servo_early_stop();
    void head_touch_init();
    void io_expander_init();
    void imu_init();
    void rtc_init();
};

Hal& GetHAL();

/**
 * @brief
 *
 */
class LvglLockGuard {
public:
    LvglLockGuard()
    {
        GetHAL().lvglLock();
    }
    ~LvglLockGuard()
    {
        GetHAL().lvglUnlock();
    }
};
