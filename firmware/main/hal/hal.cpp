/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include <memory>
#include <mooncake_log.h>
#include <nvs_flash.h>

static std::unique_ptr<Hal> _hal_instance;
static const std::string_view _tag = "HAL";

Hal& GetHAL()
{
    if (!_hal_instance) {
        mclog::tagInfo(_tag, "creating hal instance");
        _hal_instance = std::make_unique<Hal>();
    }
    return *_hal_instance.get();
}

void Hal::init()
{
    mclog::tagInfo(_tag, "init");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    servo_early_stop();
    xiaozhi_board_init();
    xiaozhi_mcp_init();
    head_touch_init();
    io_expander_init();
    rtc_init();
    imu_init();
    servo_init();
    lvgl_init();
}

/* -------------------------------------------------------------------------- */
/*                                   System                                   */
/* -------------------------------------------------------------------------- */
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <system_info.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_mac.h>

void Hal::delay(std::uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

std::uint32_t Hal::millis()
{
    return esp_timer_get_time() / 1000;
}

void Hal::feedTheDog()
{
    vTaskDelay(1);
}

std::array<uint8_t, 6> Hal::getFactoryMac()
{
    std::array<uint8_t, 6> mac;
    esp_efuse_mac_get_default(mac.data());
    return mac;
}

std::string Hal::getFactoryMacString(std::string divider)
{
    auto mac = getFactoryMac();
    return fmt::format("{:02X}{}{:02X}{}{:02X}{}{:02X}{}{:02X}{}{:02X}", mac[0], divider, mac[1], divider, mac[2],
                       divider, mac[3], divider, mac[4], divider, mac[5]);
}

void Hal::reboot()
{
    esp_restart();
}

static void _confirm_ota_image_if_stable()
{
    constexpr uint32_t ota_confirm_delay_ms = 20000;
    static bool ota_confirm_checked         = false;
    if (ota_confirm_checked || GetHAL().millis() < ota_confirm_delay_ms) {
        return;
    }
    ota_confirm_checked = true;

    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running == nullptr) {
        mclog::tagError(_tag, "failed to get running partition for ota confirmation");
        return;
    }

    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) != ESP_OK) {
        mclog::tagError(_tag, "failed to get ota state for partition: {}", running->label);
        return;
    }

    mclog::tagInfo(_tag, "ota confirm check: partition={}, state={}", running->label, static_cast<int>(ota_state));
    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        mclog::tagInfo(_tag, "ota image is stable, marking current app valid");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

void Hal::updateHeapStatusLog()
{
    _confirm_ota_image_if_stable();

    static uint32_t last_log_tick = 0;
    if (millis() - last_log_tick < 10000) {
        return;
    }
    last_log_tick = millis();
    SystemInfo::PrintHeapStats();
}

/* -------------------------------------------------------------------------- */
/*                                   Xiaozhi                                  */
/* -------------------------------------------------------------------------- */
#include "board/hal_bridge.h"
#include <stackchan/stackchan.h>
#include <stackpet/remote/remote_link.h>
#include <stackpet/ui/ui.h>
#include <apps/common/common.h>
#include <esp_heap_caps.h>
#include <assets/assets.h>

void Hal::xiaozhi_board_init()
{
    mclog::tagInfo(_tag, "xiaozhi board init");

    hal_bridge::xiaozhi_board_init();
}

// StackPet: the remote in use, its M5 button goes home from the AI agent too (its copy on every channel, heard on
// xiaozhi's WiFi). Not right at the first standby: the wake word's models load then, the internal RAM's lowest point
// (12 KB measured); a while after, with room to spare (the espnow component's task has its stack in PSRAM, patched:
// what stays internal is about 1 KB). Outside the LVGL lock: espnow_init() registers WiFi event handlers, under the
// default event loop's lock. An attach that did not take (the WiFi reconnecting) is tried again a while later
static constexpr uint32_t kRemoteAttachAfterMs = 10000;
static constexpr uint32_t kRemoteAttachRetryMs = 5000;
static constexpr uint32_t kRemoteRamCheckMs    = 1000;  // not enough: looked at again this often
static constexpr size_t kRemoteAttachMinFree   = 30 * 1024;
static constexpr size_t kRemoteAttachMinBlock  = 4 * 1024;
static constexpr uint32_t kRemoteNoticeMs      = 3000;
static bool s_remote_notice_pending            = false;  // this task only

static void remote_attach_update()
{
    static uint32_t due_ms     = 0;
    static bool done           = false;
    static bool waiting_logged = false;
    auto& link                 = stackpet::remote::RemoteLink::instance();
    if (done || !hal_bridge::is_xiaozhi_ready()) {
        return;
    }
    if (!stackpet::remote::RemoteLink::usedSincePowerOn()) {
        done = true;  // not in use: nothing to listen for (it cannot become so here, nothing is heard)
        return;
    }
    const uint32_t now = GetHAL().millis();
    if (due_ms == 0) {
        due_ms = now + kRemoteAttachAfterMs;
        return;
    }
    if (static_cast<int32_t>(now - due_ms) < 0) {
        return;
    }
    // The largest block walks every heap with interrupts off: only once the free total is enough
    const size_t free  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t block = free >= kRemoteAttachMinFree ? heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) : 0;
    if (free < kRemoteAttachMinFree || block < kRemoteAttachMinBlock) {
        if (!waiting_logged) {
            waiting_logged = true;  // the largest block measured for this line too (once)
            mclog::tagInfo(_tag, "remote: waiting for internal RAM ({} free, largest block {})", free,
                           heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        }
        due_ms = now + kRemoteRamCheckMs;
        return;
    }
    link.attach();
    if (!link.listening()) {
        due_ms = now + kRemoteAttachRetryMs;
        return;
    }
    done                    = true;
    s_remote_notice_pending = true;
    mclog::tagInfo(_tag, "remote attached: internal RAM free {} (lowest since boot {}, largest block {}), stack {}",
                   heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                   heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), uxTaskGetStackHighWaterMark(nullptr));
}

// StackPet: once it listens, said on the screen for a moment (the AI agent shows no state to go by), at the very top:
// clear of every face's eyes and brows. In Chinese if the CN font is still where it was loaded (the AI agent may have
// downloaded new assets and mapped them again since), else in English with the built-in font. With the LVGL lock
static void remote_notice_update()
{
    static lv_obj_t* notice = nullptr;
    static uint32_t shown_ms = 0;
    if (s_remote_notice_pending) {
        s_remote_notice_pending = false;
        const bool cn           = stackpet::ui::fontCn20Current();
        notice                  = lv_label_create(lv_layer_top());
        lv_obj_set_style_text_font(notice, cn ? stackpet::ui::fontCn20() : &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(notice, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_color(notice, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(notice, 160, 0);
        lv_obj_set_style_radius(notice, 12, 0);
        lv_obj_set_style_pad_hor(notice, 12, 0);
        lv_obj_set_style_pad_ver(notice, 2, 0);
        lv_label_set_text(notice, cn ? "现在可以按 M5 回桌面" : "Press M5 to go home");
        lv_obj_align(notice, LV_ALIGN_TOP_MID, 0, 4);
        shown_ms = GetHAL().millis();
    }
    if (notice != nullptr && GetHAL().millis() - shown_ms >= kRemoteNoticeMs) {
        lv_obj_delete(notice);
        notice = nullptr;
        mclog::tagInfo(_tag, "remote notice shown: stack {}", uxTaskGetStackHighWaterMark(nullptr));
    }
}

static void _stackchan_update_task(void* param)
{
    bool is_setup_done = false;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20));

        tools::update_reminders();
        remote_attach_update();  // StackPet: outside the LVGL lock

        LvglLockGuard lock;

        if (!hal_bridge::is_xiaozhi_idle()) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        GetStackChan().update();

        if (!hal_bridge::is_xiaozhi_ready()) {
            continue;
        }

        if (!is_setup_done) {
            // Setup when xiaozhi ready
            GetHAL().startSntp();
            view::create_home_indicator([]() { GetHAL().requestWarmReboot(0); }, 0x81DBBD, 0x134233);
            view::create_status_bar(0x81DBBD, 0x134233);
            is_setup_done = true;
        }

        view::update_home_indicator();
        view::update_status_bar();
        remote_notice_update();  // StackPet
    }
}

void Hal::startXiaozhi()
{
    mclog::tagInfo(_tag, "start xiaozhi");

    auto& motion = GetStackChan().motion();
    motion.setAutoAngleSyncEnabled(true);
    motion.setAutoTorqueReleaseEnabled(true);

    // Setup reminder handler
    tools::on_reminder_triggered().clear();
    tools::on_reminder_triggered().connect([](int id, std::string_view msg) {
        mclog::tagInfo(_tag, "reminder triggered: id: {}, msg: {}", id, msg);
        {
            LvglLockGuard lock;
            auto& avatar = GetStackChan().avatar();
            avatar.addDecorator(std::make_unique<view::ReminderView>(lv_screen_active(), msg));
        }
        hal_bridge::app_play_sound(OGG_NEW_NOTIFICATION);
    });

    // Start stackchan update task
    xTaskCreatePinnedToCore(_stackchan_update_task, "stackchan", 4096, NULL, 3, NULL, 1);

    hal_bridge::start_xiaozhi_app();
}

XiaozhiConfig_t Hal::getXiaozhiConfig()
{
    auto bridge_config = hal_bridge::get_xiaozhi_config();
    return XiaozhiConfig_t{
        .idleShutdownTimeSeconds   = bridge_config.idleShutdownTimeSeconds,
        .allowShutdownWhenCharging = bridge_config.allowShutdownWhenCharging,
        .idleRandomMovementLevel   = bridge_config.idleRandomMovementLevel,
        .startAiAgentOnBoot        = bridge_config.startAiAgentOnBoot,
    };
}

void Hal::setXiaozhiConfig(XiaozhiConfig_t config)
{
    hal_bridge::set_xiaozhi_config({
        .idleShutdownTimeSeconds   = config.idleShutdownTimeSeconds,
        .allowShutdownWhenCharging = config.allowShutdownWhenCharging,
        .idleRandomMovementLevel   = config.idleRandomMovementLevel,
        .startAiAgentOnBoot        = config.startAiAgentOnBoot,
    });
}

uint8_t Hal::getBatteryLevel()
{
    return hal_bridge::board_get_battery_level();
}

bool Hal::isBatteryCharging()
{
    return hal_bridge::board_is_battery_charging();
}

bool Hal::hasExternalPower()
{
    return hal_bridge::board_has_external_power();
}

void Hal::factoryReset()
{
    mclog::tagInfo(_tag, "start factory reset");
    ESP_ERROR_CHECK(nvs_flash_erase());
    reboot();
}

/* -------------------------------------------------------------------------- */
/*                                   Display                                  */
/* -------------------------------------------------------------------------- */
#include "board/hal_bridge.h"

void Hal::lvglLock()
{
    hal_bridge::disply_lvgl_lock();
}

void Hal::lvglUnlock()
{
    hal_bridge::disply_lvgl_unlock();
}

void Hal::setBackLightBrightness(uint8_t brightness, bool permanent)
{
    hal_bridge::board_set_backlight_brightness(brightness, permanent);
}

uint8_t Hal::getBackLightBrightness()
{
    return hal_bridge::board_get_backlight_brightness();
}

void Hal::setSpeakerVolume(uint8_t volume, bool permanent)
{
    hal_bridge::board_set_speaker_volume(volume, permanent);
}

uint8_t Hal::getSpeakerVolume()
{
    return hal_bridge::board_get_speaker_volume();
}

/* -------------------------------------------------------------------------- */
/*                                    Lvgl                                    */
/* -------------------------------------------------------------------------- */
#include "board/hal_bridge.h"
#include <stackchan/stackchan.h>

static void lvgl_read_cb(lv_indev_t* indev, lv_indev_data_t* data)
{
    hal_bridge::lock();
    auto& bridge_data = hal_bridge::get_data();

    // mclog::tagInfo(_tag, "touchpoint: {}, x: {}, y: {}", bridge_data.touchPoint.num, bridge_data.touchPoint.x,
    //                bridge_data.touchPoint.y);

    if (bridge_data.touchPoint.num == 0) {
        data->state = LV_INDEV_STATE_RELEASED;
    } else {
        data->state   = LV_INDEV_STATE_PRESSED;
        data->point.x = bridge_data.touchPoint.x;
        data->point.y = bridge_data.touchPoint.y;
    }

    hal_bridge::unlock();
}

void Hal::lvgl_init()
{
    mclog::tagInfo(_tag, "lvgl init");

    hal_bridge::disply_lvgl_lock();

    mclog::tagInfo(_tag, "create lvgl touchpad indev");
    lvTouchpad = lv_indev_create();
    lv_indev_set_type(lvTouchpad, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(lvTouchpad, lvgl_read_cb);
    lv_indev_set_group(lvTouchpad, lv_group_get_default());
    lv_indev_set_display(lvTouchpad, hal_bridge::display_get_lvgl_display());

    hal_bridge::disply_lvgl_unlock();
}

/* -------------------------------------------------------------------------- */
/*                                 Warm Reboot                                */
/* -------------------------------------------------------------------------- */
#include <settings.h>
#include <string_view>

static std::string_view _warm_boot_nvs_ns  = "warm_boot";
static std::string_view _warm_boot_nvs_key = "app_index";

void Hal::requestWarmReboot(int appIndex)
{
    mclog::tagInfo(_tag, "warm reboot request to app index: {}", appIndex);

    {
        Settings settings(_warm_boot_nvs_ns.data(), true);
        settings.SetInt(_warm_boot_nvs_key.data(), appIndex);
    }

    delay(100);
    esp_restart();
}

int Hal::getWarmRebootTarget()
{
    Settings settings(_warm_boot_nvs_ns.data(), false);
    return settings.GetInt(_warm_boot_nvs_key.data(), -1);
}

void Hal::clearWarmRebootRequest()
{
    mclog::tagInfo(_tag, "clear warm reboot request");

    Settings settings(_warm_boot_nvs_ns.data(), true);
    settings.SetInt(_warm_boot_nvs_key.data(), -1);
}
