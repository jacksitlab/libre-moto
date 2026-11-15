/* ioexp_pcf8574.c — PCF8574 I/O expander over I2C, ESP-IDF.
 *
 * Same protocol as the Arduino PCF8574 library: one read/write byte,
 * quasi-bidirectional ports. Drives LCD power/reset + touch reset/INT
 * on the CrowPanel (P0=TP reset, P2=TP INT, P3=LCD power, P4=LCD reset).
 */

#include "ioexp_pcf8574.h"

#include "config.h"

/* Port bit numbers (PCF8574 P0..P7). The shared config.h uses the same
 * macro names the Arduino PCF8574 library defines; the ESP-IDF port
 * supplies them here (guarded — the sketch build gets them from the
 * library header and must not see a redefinition). */
#ifndef P0
#define P0 0
#define P1 1
#define P2 2
#define P3 3
#define P4 4
#define P5 5
#define P6 6
#define P7 7
#endif

#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"

static const char *TAG = "pcf8574";

static i2c_master_dev_handle_t s_dev = NULL;
static uint8_t s_shadow = 0;   /* last written output byte */

bool pcf8574_init(i2c_master_bus_handle_t bus, uint8_t addr) {
    /* The probe can intermittently fail right after power-up (one 100 ms
     * window was not enough across boots — the expander showed up in one
     * boot and not the next). Retry a few times before giving up. */
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 5; ++attempt) {
        err = i2c_master_probe(bus, addr, 200);
        if (err == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PCF8574 not found at 0x%02X (%s)", addr, esp_err_to_name(err));
        return false;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
        .scl_wait_us = 0,
        .flags = { .disable_ack_check = 0 },
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &s_dev));
    ESP_LOGI(TAG, "PCF8574 at 0x%02X ready", addr);
    return true;
}

bool pcf8574_write_bit(int bit, int value) {
    if (!s_dev || bit < 0 || bit > 7) return false;
    if (value) s_shadow |= (uint8_t)(1u << bit);
    else       s_shadow &= (uint8_t)~(1u << bit);
    esp_err_t err = i2c_master_transmit(s_dev, &s_shadow, 1, 100);
    return err == ESP_OK;
}

/* CrowPanel power/reset sequencing (same order/delays as the sketch's
 * setup()): LCD power on, LCD reset pulse, touch reset pulse, TP INT
 * pull-up. Runs BEFORE the ST7701 init stream. */
void pcf8574_panel_power_sequence(void) {
    pcf8574_write_bit(PCF_LCD_POWER, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    pcf8574_write_bit(PCF_LCD_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    pcf8574_write_bit(PCF_LCD_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
    pcf8574_write_bit(PCF_LCD_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    pcf8574_write_bit(PCF_TP_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    pcf8574_write_bit(PCF_TP_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
    pcf8574_write_bit(PCF_TP_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    pcf8574_write_bit(PCF_TP_INT, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}
