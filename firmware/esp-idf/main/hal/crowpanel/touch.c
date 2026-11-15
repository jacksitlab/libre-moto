/* touch_cst816.c — CST816 capacitive touch over I2C, ESP-IDF.
 *
 * Native port of Adafruit_CST8XX (same register protocol):
 *   reg 0x02 = number of touches, reg 0x03.. = 6 bytes per point
 *   (event/x-hi|x-lo/y-hi|id/y-lo).
 * The sketch reads touched()/getPoint(0) each loop; here the same two
 * calls exist as touch_touched()/touch_get_point().
 */

#include "touch.h"

#include "config.h"

#include "esp_log.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch";

#define CST816_ADDR       I2C_TOUCH_ADDR
#define REG_NUMTOUCHES    0x02
#define REG_TOUCHDATA     0x03
#define CST816_CHIPID_REG 0xAA
#define CST816_CHIPID     0xB4   /* CST816S */

static i2c_master_dev_handle_t s_dev = NULL;

bool touch_init(i2c_master_bus_handle_t bus) {
    esp_err_t err = i2c_master_probe(bus, CST816_ADDR, 100);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CST816 not found at 0x%02X (probe: %s)",
                 CST816_ADDR, esp_err_to_name(err));
        return false;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CST816_ADDR,
        .scl_speed_hz = 400000,
        .scl_wait_us = 0,
        .flags = { .disable_ack_check = 0 },
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &s_dev));

    uint8_t id = 0;
    uint8_t reg = CST816_CHIPID_REG;
    i2c_master_transmit_receive(s_dev, &reg, 1, &id, 1, 100);
    ESP_LOGI(TAG, "CST816 chip id 0x%02X", id);
    return true;
}

bool touch_touched(void) {
    if (!s_dev) return false;
    uint8_t n = 0;
    uint8_t reg = REG_NUMTOUCHES;
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, &n, 1, 50);
    if (err != ESP_OK) return false;
    if (n > 5) return false;
    return n > 0;
}

void touch_get_point(int16_t *x, int16_t *y) {
    *x = 0;
    *y = 0;
    if (!s_dev) return;
    uint8_t reg = REG_TOUCHDATA;
    uint8_t d[6] = {0};
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, d, sizeof d, 50);
    if (err != ESP_OK) return;
    *x = (int16_t)(((d[0] & 0x0F) << 8) | d[1]);
    *y = (int16_t)(((d[2] & 0x0F) << 8) | d[3]);
}
