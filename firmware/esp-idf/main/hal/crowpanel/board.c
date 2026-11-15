/* board.c — CrowPanel 2.1" bring-up.
 *
 * Owns the board-specific power chain: PCF8574 expander (LCD power,
 * LCD reset, touch reset) + LEDC backlight on GPIO 6. app_main calls
 * board_power_init() before panel_init() and never touches the
 * expander itself.
 */
#include "board.h"
#include "ioexp_pcf8574.h"
#include "backlight.h"
#include "config.h"
#include "esp_log.h"

static const char *TAG = "board";

bool board_power_init(i2c_master_bus_handle_t i2c_bus) {
    /* PCF8574: LCD power/reset + touch reset (must run BEFORE panel init) */
    if (!pcf8574_init(i2c_bus, PCF8574_ADDR)) {
        ESP_LOGE(TAG, "PCF8574 missing - display power sequencing skipped");
        return false;
    }
    pcf8574_panel_power_sequence();

    /* Backlight LEDC channel — must be initialized BEFORE the first
     * backlight_set() call from app_main (the restructure dropped the
     * explicit init and LEDC rejected every set_duty). */
    backlight_init();
    return true;
}
