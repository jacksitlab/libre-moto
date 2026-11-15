/* board.c — Waveshare ESP32-S3-Touch-AMOLED-1.75C bring-up.
 *
 * STATUS: stub (board not yet in hand). No PCF8574, no backlight
 * pin — all contract functions are no-ops so app_main compiles and
 * runs unchanged; the CO5300 panel stub reports NOT_SUPPORTED.
 */
#include "board.h"
#include "esp_log.h"

static const char *TAG = "board";

bool board_power_init(i2c_master_bus_handle_t i2c_bus) {
    (void)i2c_bus;
    ESP_LOGW(TAG, "Waveshare board power sequencing: nothing to do (no expander)");
    return true;
}

void backlight_init(void) { /* AMOLED: no backlight pin */ }
void backlight_set(int pct) { (void)pct; }
int  backlight_get(void) { return 100; }
