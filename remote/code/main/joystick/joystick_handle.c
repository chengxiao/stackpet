/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "joystick_handle.h"
#include "driver/gpio.h"

i2c_bus_device_handle_t i2c_device1;  // i2c device handle

/* StackPet: the I2C lines' levels before the bus is set up, only read (SCL low: the JoyC is stuck, it holds the clock
 * after a reset of the remote that disturbed the bus: GPIO0, its SDA, is also the boot pin the USB chip drives; only
 * a power cycle of the whole remote helps then). Driving the lines here to free the bus was tried: it made things
 * worse, a JoyC fresh from a power cycle came up stuck. */
static void joystick_i2c_lines(void)
{
    gpio_set_direction(GPIO_NUM_0, GPIO_MODE_INPUT);
    gpio_set_direction(GPIO_NUM_26, GPIO_MODE_INPUT);
    ESP_LOGI("StackPet", "I2C lines: SDA %d SCL %d", gpio_get_level(GPIO_NUM_0), gpio_get_level(GPIO_NUM_26));
}

/**
 * @brief Initialize joystick via I2C interface
 * @note This is an internal static function that configures I2C_NUM_0 as master with SDA on GPIO0 and SCL on GPIO26
 * @details
 *      1. Configures I2C master mode with 100kHz clock speed
 *      2. Creates I2C bus handle using I2C_NUM_0
 *      3. Scans the I2C bus to detect connected devices and logs their addresses
 *      4. Creates device handle for joystick at I2C address 0x54
 *      5. Assigns the device handle to global variable [i2c_device1]
 * @warning This function assumes the joystick device is at I2C address 0x54
 */
static void joystick_i2c_init()
{
    joystick_i2c_lines();  // StackPet
    i2c_config_t conf;
    {
        conf.mode             = I2C_MODE_MASTER;
        conf.sda_io_num       = 0;
        conf.scl_io_num       = 26;
        conf.sda_pullup_en    = GPIO_PULLUP_ENABLE;
        conf.scl_pullup_en    = GPIO_PULLUP_ENABLE;
        conf.master.clk_speed = 100000;
        conf.clk_flags        = 0;
    };
    i2c_bus_handle_t i2c0_bus = i2c_bus_create(I2C_NUM_0, &conf);
    // StackPet: no scan of every address for the log (with the JoyC stuck that took 126 x 200 ms = 25 s); the first
    // read of its button (joystick_button_init()) says whether it answers
    i2c_device1 = i2c_bus_device_create(i2c0_bus, 0x54, 0);
}

/* StackPet: the stick's button (Mini JoyC register 0x30: 0 while pressed, as M5Stack's MiniJoyHat driver reads it) and
 * its RGB LED (0x40, R G B). A read that fails keeps the last state.
 * What the running screen shows about the JoyC: its button register as last read (-1: never), and how many reads in a
 * row failed (it stops answering when it is stuck, see joystick_i2c_lines()). */
int stackpet_joyc_button_raw       = -1;
int stackpet_joyc_button_err       = 0;  // the last button read's esp_err_t (0: fine)
uint32_t stackpet_joyc_fail_streak = 0;

static bool s_button_pressed = false;
static int s_button_raw      = -1;  // for the log: the register's value last read
static uint32_t s_button_errors = 0;
static uint32_t s_xy_errors     = 0;

static void joystick_set_led(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t rgb[3] = {r, g, b};
    i2c_bus_write_bytes(i2c_device1, 0x40, 3, rgb);
}

// StackPet: stuck (stackpet_joyc_fail_streak), the JoyC is only tried every 2 s: every try waits 200 ms for nothing
static bool joystick_skip_stuck(void)
{
    static TickType_t retry_at = 0;
    if (stackpet_joyc_fail_streak < 20) {
        return false;
    }
    const TickType_t now = xTaskGetTickCount();
    if ((int32_t)(now - retry_at) < 0) {
        return true;
    }
    retry_at = now + pdMS_TO_TICKS(2000);
    return false;
}

/* StackPet: a register read the way M5Stack's own drivers do it (M5HatMiniJoyC::readBytes(), Wire): the register's
 * address in a write of its own, with a stop, then a read of its own, with Wire's clock stretch timeout. */
static esp_err_t joystick_read_register(uint8_t reg, uint8_t *data, size_t len)
{
    // The JoyC holds the clock low a while before it answers its button register: longer than the legacy driver's
    // default stretch timeout on the ESP32 (10 bus cycles, 100 us at 100 kHz: every read failed with
    // ESP_ERR_TIMEOUT). Arduino's Wire, which M5Stack's drivers use, sets the longest the hardware has; so do we
    // (0xFFFFF APB cycles, ~13 ms), before every read: i2c_bus sets the port up again when a device's settings differ
    i2c_set_timeout(I2C_NUM_0, 0xFFFFF);
    esp_err_t err = i2c_bus_write_bytes(i2c_device1, NULL_I2C_MEM_ADDR, 1, &reg);
    if (err == ESP_OK) {
        err = i2c_bus_read_bytes(i2c_device1, NULL_I2C_MEM_ADDR, len, data);
    }
    return err;
}

static void joystick_button_init(void)
{
    uint8_t value       = 0xFF;
    const esp_err_t err = joystick_read_register(0x30, &value, 1);
    ESP_LOGI("StackPet", "JoyC stick button (0x30): %s, %u", esp_err_to_name(err), value);
    joystick_set_led(0, 0, 0);
}

static uint8_t joystick_read_buttons(void)
{
    if (joystick_skip_stuck()) {
        return s_button_pressed ? 0x01 : 0x00;
    }
    // A moment after the stick's reads, as between those two (the JoyC wants it, says the original code)
    vTaskDelay(10 / portTICK_PERIOD_MS);
    uint8_t value       = 0xFF;
    const esp_err_t err = joystick_read_register(0x30, &value, 1);
    stackpet_joyc_button_err = err;
    if (err == ESP_OK) {
        stackpet_joyc_button_raw  = value;
        stackpet_joyc_fail_streak = 0;
        if (value != s_button_raw) {
            s_button_raw = value;
            ESP_LOGI("StackPet", "stick button register 0x30 = %u", value);
        }
        const bool pressed = value == 0;
        if (pressed != s_button_pressed) {
            s_button_pressed = pressed;
            ESP_LOGI("StackPet", "stick %s", pressed ? "pressed" : "released");
            joystick_set_led(0, 0, pressed ? 80 : 0);  // lit while pressed
        }
    } else {
        stackpet_joyc_fail_streak++;
        if (s_button_errors++ % 100 == 0) {
            ESP_LOGW("StackPet", "stick button read failed: %s (%lu times)", esp_err_to_name(err),
                     (unsigned long)s_button_errors);
        }
    }
    return s_button_pressed ? 0x01 : 0x00;
}

/* StackPet: the M5 button in the packet's buttons byte (bit 1): held now, or clicked since the last look (a click
 * shorter than a turn of the loop would be missed otherwise; the next turn sends it let go) */
static uint8_t joystick_home_bit(joystick_data_t *joystick_data)
{
    const bool clicked = __atomic_exchange_n(&joystick_data->home_click, 0u, __ATOMIC_ACQ_REL) != 0;
    return (joystick_data->home_held || clicked) ? 0x02 : 0x00;
}

/* StackPet: the buttons byte now (the stick's bit, the M5 button's); true when the M5 button just went down: its count
 * goes up, and that packet goes out once more on every channel (joystick_send_everywhere()) */
static bool joystick_update_buttons(joystick_data_t *joystick_data, uint8_t stick_bits)
{
    const uint8_t buttons = stick_bits | joystick_home_bit(joystick_data);
    const bool pressed    = (buttons & 0x02) != 0 && (joystick_data->buttons & 0x02) == 0;
    if (pressed) {
        joystick_data->home_count++;
    }
    joystick_data->buttons = buttons;
    return pressed;
}

/* StackPet: the packet just sent, once more on every channel, marked as that copy (bit 2): a robot whose app keeps
 * it on its router's channel hears the M5 press there (it listens for nothing else there) */
static void joystick_send_everywhere(uint8_t pkt[11])
{
    pkt[8] |= 0x04;
    espnow_send_everywhere(pkt, 11);
    pkt[8] &= (uint8_t)~0x04;
}

/* StackPet: every packet made from what the remote has now, also when only a button changed (the id may have been
 * changed on the setup screen since the last move, and nothing may have moved yet in this mode) */
static void fill_packet(uint8_t pkt[11], const joystick_data_t *joystick_data, int16_t yaw_angle, int16_t pitch_angle,
                        int16_t speed_val)
{
    pkt[0] = joystick_data->id;
    memcpy(&pkt[1], &yaw_angle, sizeof(int16_t));
    memcpy(&pkt[3], &pitch_angle, sizeof(int16_t));
    memcpy(&pkt[5], &speed_val, sizeof(int16_t));
    pkt[7] = joystick_data->btnB_status;
    pkt[8]  = joystick_data->buttons;
    pkt[9]  = (uint8_t)joystick_data->channel;
    pkt[10] = joystick_data->home_count;
}

/**
 * @brief Read X and Y axis values from the joystick via I2C
 * @param joyX Pointer to store X-axis value (16-bit unsigned integer)
 * @param joyY Pointer to store Y-axis value (16-bit unsigned integer)
 * @return void
 * @details
 *      1. Reads 2 bytes from register address 0x00 (X-axis low/high bytes)
 *      2. Waits 10ms to ensure data stability
 *      3. Reads 2 bytes from register address 0x02 (Y-axis low/high bytes)
 *      4. Combines high and low bytes for both X and Y axes using bit shifting
 *      5. Stores the combined values in the provided pointers
 * @warning This function assumes the joystick provides 16-bit data in little-endian format
 */
static void joystick_read_xy(uint16_t *joyX, uint16_t *joyY)
{
    if (joystick_skip_stuck()) {
        return;  // StackPet: the last position stays
    }
    uint8_t data[4];
    esp_err_t ret = i2c_bus_read_bytes(i2c_device1, 0x00, 2, data);
    vTaskDelay(20 / portTICK_PERIOD_MS);
    ret |= i2c_bus_read_bytes(i2c_device1, 0x02, 2, &data[2]);
    if (ret == ESP_OK) {
        *joyX                     = (data[1] << 8) | data[0];
        *joyY                     = (data[3] << 8) | data[2];
        stackpet_joyc_fail_streak = 0;
    } else {
        // StackPet: how often (the stick keeps its last position meanwhile)
        stackpet_joyc_fail_streak++;
        if (s_xy_errors++ % 100 == 0) {
            ESP_LOGW("StackPet", "stick position read failed (%lu times)", (unsigned long)s_xy_errors);
        }
    }
}

/**
 * @brief Public interface to initialize the joystick and return default configuration
 * @return joystick_data_t Structure containing initialized joystick parameters
 * @note This is the main initialization function exposed to users
 * @details
 *      1. Calls internal： device_joystick_init()
 *      2. Initializes all fields of 'joystick_data_t'
 *         - channel: 1 (default communication channel)
 *         - id: 0 (default target ID)
 *         - bat: 0 (battery level, to be updated later)
 *         - joyX, joyY: 0 (initial joystick positions)
 *         - screen_mode: MODE_SETUP (start in setup mode)
 *         - select_mode: CHANNEL_SELECT (default selection mode)
 * @return joystick_data_t
 */
joystick_data_t joystick_init()
{
    joystick_i2c_init();
    joystick_button_init();
    joystick_data_t tmp;
    tmp.channel     = 1;
    tmp.id          = 0;
    tmp.bat         = 0;
    tmp.joyX        = X_CENTER;  // StackPet: the middle until it is read (0 maps to yaw 1800, pitch -88)
    tmp.joyY        = Y_CENTER;
    tmp.accel_x     = 0.0f;
    tmp.accel_y     = 0.0f;
    tmp.accel_z     = 0.0f;
    tmp.screen_mode = MODE_RUNNING;  // StackPet: starts on the running screen (ui_init)
    tmp.select_mode = CHANNEL_SELECT;
    tmp.btnB_status = false;
    tmp.home_held   = false;
    tmp.home_click  = 0;
    tmp.home_count  = 0;
    tmp.buttons     = 0;
    return tmp;
}

/**
 * @brief Task to handle joystick setup screen
 * @param pvParam Pointer to joystick data, pointing to joystick_data_t structure
 * @note This function runs an infinite loop that continuously reads joystick XY coordinates
 *       and updates the setup screen when the screen mode is MODE_SETUP
 * @details Reads raw joystick data and then calls update_setup_screen function to update screen display
 *          Each loop iteration has a 50ms delay to ensure interface responsiveness
 */
void handle_setup_screen(void *pvParam)
{
    joystick_data_t *joystick_data = (joystick_data_t *)pvParam;
    while (1) {
        if (joystick_data->screen_mode == MODE_SETUP) {
            joystick_read_xy(&joystick_data->joyX, &joystick_data->joyY);
            update_setup_screen(joystick_data);
            vTaskDelay(50 / portTICK_PERIOD_MS);
        } else {
            vTaskDelay(200 / portTICK_PERIOD_MS);
        }
    }
}

/**
 * @brief Task to handle joystick running screen, responsible for reading joystick data and sending ESP-NOW control
 * packets
 * @param pvParam Pointer to joystick data, pointing to joystick_data_t structure
 * @note This function runs an infinite loop that reads joystick input, processes data, and sends control packets in
 * running mode
 * @details
 *      1. Reads raw X/Y values from the joystick via I2C
 *      2. Updates the running screen display with current values
 *      3. Applies deadzone correction to center the joystick values
 *      4. Maps raw values to yaw/pitch angle ranges (-1280 to 1280 for yaw, 0 to 900 for pitch)
 *      5. Only sends data when changes exceed threshold (5 units) to reduce network traffic
 *      6. Constructs and sends ESP-NOW packet containing target ID, yaw, pitch, speed and button status
 *      7. Each loop iteration has a 30ms delay when in running mode
 */
void handle_running_screen(void *pvParam)
{
    joystick_data_t *joystick_data = (joystick_data_t *)pvParam;
    // communicate packet (StackPet: three more bytes, the buttons, the channel sent on and the M5 button's count; robots
    // that know 8 bytes only ignore them)
    uint8_t pkt[11]     = {0};
    pkt[0]              = joystick_data->id;  // id: 0 for broadcast
    int16_t yaw_angle   = 0;
    int16_t pitch_angle = 0;
    int16_t last_yaw    = 0;
    int16_t last_pitch  = 0;
    int16_t speed_val   = 600;

    while (1) {
        // update screen and send packet when in running mode
        if (joystick_data->screen_mode == MODE_RUNNING) {
            joystick_read_xy(&joystick_data->joyX, &joystick_data->joyY);
            const bool home_pressed = joystick_update_buttons(joystick_data, joystick_read_buttons());

            if (running_screen != NULL && lv_obj_is_valid(running_screen)) {
                update_running_screen(joystick_data->joyX, joystick_data->joyY, joystick_data->channel,
                                      joystick_data->id, joystick_data->bat);
            }

            // handle data from joystick
            if ((joystick_data->joyX < X_CENTER + DEAD_ZONE) && (joystick_data->joyX > X_CENTER - DEAD_ZONE)) {
                joystick_data->joyX = X_CENTER;
            }
            if ((joystick_data->joyY < Y_CENTER + DEAD_ZONE) && (joystick_data->joyY > Y_CENTER - DEAD_ZONE)) {
                joystick_data->joyY = Y_CENTER;
            }

            yaw_angle   = (int16_t)map(joystick_data->joyX, X_MIN, X_MAX, 1280, -1280);
            pitch_angle = (int16_t)map(joystick_data->joyY, Y_MIN, Y_MAX, 0, 900);

            // send pitch_angle and yaw_angle only when changes exceed threshold
            if (abs(yaw_angle - last_yaw) < 5 && abs(pitch_angle - last_pitch) < 5) {
                if (pkt[7] != joystick_data->btnB_status || pkt[8] != joystick_data->buttons) {
                    fill_packet(pkt, joystick_data, yaw_angle, pitch_angle, speed_val);
                    espnow_send_data(pkt, sizeof(pkt));
                    if (home_pressed) {
                        joystick_send_everywhere(pkt);
                    }
                }
                vTaskDelay(30 / portTICK_PERIOD_MS);
                continue;
            }

            fill_packet(pkt, joystick_data, yaw_angle, pitch_angle, speed_val);

#if 0 
            ESP_LOGI("handle_running_screen", "Yaw: %d, Pitch: %d, Speed: %d, id: %u, Button: %u", 
                        yaw_angle, pitch_angle, speed_val, joystick_data->id, joystick_data->btnB_status);
#endif

            last_yaw   = yaw_angle;
            last_pitch = pitch_angle;
            espnow_send_data(pkt, sizeof(pkt));
            if (home_pressed) {
                joystick_send_everywhere(pkt);
            }
            vTaskDelay(30 / portTICK_PERIOD_MS);
        } else {
            vTaskDelay(200 / portTICK_PERIOD_MS);
        }
    }
}

/**
 * @brief Task to handle joystick IMU screen functionality, processing IMU sensor data and sending ESP-NOW control
 * packets
 *
 * This function runs an infinite loop that reads IMU sensor data (accelerometer and gyroscope),
 * updates the IMU visualization screen, processes the angle data to control remote devices,
 * and sends ESP-NOW packets with the processed control information.
 * The function maps the IMU pitch and roll angles to yaw and pitch values for remote control,
 * applies filtering to reduce unnecessary transmissions, and sends control packets at regular intervals.
 *
 * @param pvParam Pointer to joystick data structure containing IMU sensor values, battery level,
 *                device ID, communication channel, and other control parameters
 * @details
 *      1. Continuously reads IMU data (acceleration and gyro values) from the joystick_data structure
 *      2. Updates the IMU screen visualization with current sensor values
 *      3. Limits roll values to range [-1.5, 1.5] and pitch values to range [0, 1.5]
 *      4. Maps limited angle values to appropriate yaw/pitch ranges for remote control (-1280 to 1280 for yaw, 900 to 0
 * for pitch)
 *      5. Only sends control packets when changes exceed threshold (10 units) to minimize network traffic
 *      6. Constructs and transmits ESP-NOW packet with device ID, yaw, pitch, speed, and button status
 *      7. Includes a 50ms delay between iterations when in IMU mode, 200ms otherwise
 */
void handle_imu_screen(void *pvParam)
{
    joystick_data_t *joystick_data = (joystick_data_t *)pvParam;

    static IMU_Angle_t last_imu_angle = {0.0f, 0.0f};

    // communicate packet (StackPet: with the buttons, channel and count bytes, as in the running screen)
    uint8_t pkt[11]     = {0};
    pkt[0]              = joystick_data->id;  // id: 0 for broadcast
    int16_t yaw_angle   = 0;
    int16_t pitch_angle = 0;
    int16_t last_yaw    = 0;
    int16_t last_pitch  = 0;
    int16_t speed_val   = 600;

    while (1) {
        // update screen and send packet when in running mode
        if (joystick_data->screen_mode == MODE_IMU) {
            // StackPet: the JoyC is not read here (as before), so no stick press; the M5 button all the same
            const bool home_pressed = joystick_update_buttons(joystick_data, 0);
            IMU_Angle_t imu_angle =
                update_imu_screen(joystick_data->accel_x, joystick_data->accel_y, joystick_data->accel_z,
                                  joystick_data->bat, joystick_data->id, joystick_data->channel);

            // Limit the roll value to the range of -1.5 to 1.5
            float limited_roll = fmaxf(-1.5f, fminf(1.5f, imu_angle.roll));
            // Limit the pitch value to the range of 0 to 1.5
            float limited_pitch = fmaxf(0.0f, fminf(1.5f, imu_angle.pitch));

            yaw_angle   = (int16_t)map(limited_roll, -1.5, 1.5, -1280, 1280);
            pitch_angle = (int16_t)map(limited_pitch, 0, 1.5, 900, 0);

            if (abs(yaw_angle - last_yaw) < 10 && abs(last_pitch - pitch_angle) < 10) {
                if (pkt[7] != joystick_data->btnB_status || pkt[8] != joystick_data->buttons) {
                    fill_packet(pkt, joystick_data, yaw_angle, pitch_angle, speed_val);
                    espnow_send_data(pkt, sizeof(pkt));
                    if (home_pressed) {
                        joystick_send_everywhere(pkt);
                    }
                }
                vTaskDelay(30 / portTICK_PERIOD_MS);
                continue;
            }
            last_yaw   = yaw_angle;
            last_pitch = pitch_angle;

            fill_packet(pkt, joystick_data, yaw_angle, pitch_angle, speed_val);
            espnow_send_data(pkt, sizeof(pkt));
            if (home_pressed) {
                joystick_send_everywhere(pkt);
            }

#if 0 
            // ESP_LOGI("handle_imu_screen", "yaw_angle: %.2f, pitch_angle:%.2f, yaw: %d, pitch: %d\n", 
                                            imu_angle.roll, imu_angle.pitch, yaw_angle, pitch_angle);
#endif

            vTaskDelay(30 / portTICK_PERIOD_MS);
        } else {
            vTaskDelay(200 / portTICK_PERIOD_MS);
        }
    }
}