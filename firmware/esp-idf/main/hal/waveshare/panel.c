/* panel_co5300.c — CO5300 QSPI AMOLED panel driver, ESP-IDF.
 *
 * STATUS: stub — does not bring a panel up. Kept so the LM_BOARD=waveshare
 * CMake path compiles and the board bring-up has a concrete place to land.
 *
 * Implementation notes (from the Waveshare wiki, ESP32-S3-Touch-AMOLED-1.75C):
 *   - 466x466 round AMOLED, CO5300 controller over QSPI
 *     (SIO0=GPIO4, SIO1=GPIO5, SIO2=GPIO6, SIO3=GPIO7, SCLK=GPIO38,
 *      CS=GPIO12, RESET=GPIO1)
 *   - esp_lcd offers esp_lcd_new_panel_io_spi (QSPI flags) + a vendor
 *     CO5300 panel driver path; the framebuffer contract differs from
 *     the RGB bus: QSPI panels are written per-frame via
 *     esp_lcd_panel_draw_bitmap, there is NO continuously-scanned DMA fb.
 *     panel_init must therefore allocate the 466x466x2 B fb itself
 *     (PSRAM) and panel_get_framebuffer() returns it; draw happens in
 *     the flush path, not by the hardware scanning memory.
 *   - No backlight pin on this board (AMOLED self-emissive).
 */

#include "panel.h"
#include "esp_log.h"

static const char *TAG = "panel";

esp_err_t panel_init(esp_lcd_panel_handle_t *out_panel, uint16_t **out_fb) {
    ESP_LOGW(TAG, "CO5300 (Waveshare AMOLED 1.75C) driver not implemented yet");
    return ESP_ERR_NOT_SUPPORTED;
}

uint16_t *panel_get_framebuffer(void) {
    return NULL;
}
