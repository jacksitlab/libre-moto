/* backlight.c — PWM backlight via LEDC, ESP-IDF.
 * Same duty math as the sketch: pct * 255 / 100 at 5 kHz / 8 bit. */

#include "backlight.h"

#include "esp_err.h"
#include "config.h"
#include "esp_log.h"
#include "esp_idf_version.h"
#if ESP_IDF_VERSION_MAJOR >= 6
#include "driver/ledc.h"        /* esp_driver_ledc component (driver/ was split in v6) */
#else
#include "driver/ledc.h"
#endif

static const char *TAG = "backlight";

#define LEDC_TIMER    LEDC_TIMER_0
#define LEDC_CHANNEL  LEDC_CHANNEL_0
#define LEDC_FREQ_HZ  PWM_FREQ
#define LEDC_RESOLUTION LEDC_TIMER_8_BIT

static int s_pct = 70;

void backlight_init(void) {
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_RESOLUTION,
        .timer_num = LEDC_TIMER,
        .freq_hz = LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));
    ledc_channel_config_t ch = {
        .gpio_num = SCREEN_BACKLIGHT_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
    ESP_LOGI(TAG, "LEDC backlight on GPIO %d (%d Hz, 8-bit)", SCREEN_BACKLIGHT_PIN, LEDC_FREQ_HZ);
}

void backlight_set(int pct) {
    if (pct < BRIGHTNESS_MIN_PCT) pct = BRIGHTNESS_MIN_PCT;
    if (pct > 100) pct = 100;
    s_pct = pct;
    uint32_t duty = ((uint32_t)pct * 255) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL);
}

int backlight_get(void) { return s_pct; }
