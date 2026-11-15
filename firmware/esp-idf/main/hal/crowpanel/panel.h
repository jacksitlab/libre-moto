/* panel_st7701.h — CrowPanel 2.1" RGB panel (ST7701S) driver, ESP-IDF. */
#ifndef LM_PANEL_H
#define LM_PANEL_H

#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the panel: bitbang init sequence + RGB peripheral with a
 * PSRAM framebuffer. Returns the panel handle and the DMA-scanned fb. */
esp_err_t panel_init(esp_lcd_panel_handle_t *out_panel, uint16_t **out_fb);

/* The framebuffer the panel hardware scans (PSRAM, cache-writeback
 * handled by esp_lcd). Same role as gfx->getFramebuffer() in the sketch. */
uint16_t *panel_get_framebuffer(void);

#ifdef __cplusplus
}
#endif

#endif