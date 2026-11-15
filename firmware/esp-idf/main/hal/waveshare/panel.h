/* panel_co5300.h — CO5300 QSPI AMOLED panel driver, ESP-IDF.
 *
 * Waveshare ESP32-S3-Touch-AMOLED-1.75C display frontend.
 *
 * The board exposes the same panel contract as the CrowPanel's ST7701
 * driver: panel_init() brings the display up and returns the DMA-scanned
 * framebuffer, panel_get_framebuffer() returns it afterwards. The app
 * code (app_main.cpp) does not know which board it runs on.
 *
 * STATUS: stub. The CO5300 needs a QSPI (esp_lcd IO) data path instead
 * of the ST7701's RGB parallel bus — implementation pending until the
 * board arrives. Pins (Waveshare wiki, ESP32-S3-Touch-AMOLED-1.75C):
 *   SIO0=GPIO4  SIO1=GPIO5  SIO2=GPIO6  SIO3=GPIO7
 *   SCLK=GPIO38  CS=GPIO12  RESET=GPIO1, 466x466 round AMOLED.
 */
#ifndef LM_PANEL_H
#define LM_PANEL_H

#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t panel_init(esp_lcd_panel_handle_t *out_panel, uint16_t **out_fb);
uint16_t *panel_get_framebuffer(void);

#ifdef __cplusplus
}
#endif

#endif
