/* touch_cst9217.c — CST9217 touch driver (I2C), ESP-IDF.
 *
 * STATUS: stub — reports "no touch" so the gesture loop stays idle and
 * serial commands keep working on an unconfigured board.
 */

#include "touch.h"
#include "esp_log.h"

static const char *TAG = "touch";

bool touch_init(i2c_master_bus_handle_t bus) {
    ESP_LOGW(TAG, "CST9217 (Waveshare AMOLED 1.75C) driver not implemented yet");
    return false;
}

bool touch_touched(void) {
    return false;
}

void touch_get_point(int16_t *x, int16_t *y) {
    if (x) *x = 0;
    if (y) *y = 0;
}
