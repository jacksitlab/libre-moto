/* splash.c — boot splash painted straight into the panel framebuffer.
 *
 * Same approach as the sketch (LVGL must NOT render a full screen in
 * DIRECT mode during boot — it hangs): fill black + blit the I8 logo
 * (palette + indices), R/B-swapped to panel byte order.
 */

#include "splash.h"

#include <string.h>
#include "config.h"
#include "esp_log.h"
#include "panel.h"
#include "libremoto_logo.h"

static const char *TAG = "splash";

void splash_paint(uint16_t *panel_fb) {
    if (!panel_fb) return;
    for (size_t i = 0; i < (size_t)SCREEN_W * SCREEN_H; i++) panel_fb[i] = 0x0000;

    /* logo: 480x188 I8 — palette (256 ARGB8888, B,G,R,A) + indices */
    const uint8_t *pal = libremoto_logo_map;
    const uint8_t *idx = libremoto_logo_map + 256 * 4;
    const int lw = 480, lh = 188;
    const int x0 = (SCREEN_W - lw) / 2, y0 = (SCREEN_H - lh) / 2;
    for (int y = 0; y < lh; y++) {
        uint16_t *dst = panel_fb + (size_t)(y0 + y) * SCREEN_W + x0;
        const uint8_t *row = idx + (size_t)y * lw;
        for (int x = 0; x < lw; x++) {
            const uint8_t *e = pal + row[x] * 4;   /* B,G,R,A */
            const uint8_t b = e[0], g = e[1], r = e[2], a = e[3];
            if (a < 16) { dst[x] = 0x0000; continue; }
            /* panel byte order: R/B swapped */
            dst[x] = (uint16_t)(((uint32_t)b >> 3) << 11 |
                                ((uint32_t)g >> 2) << 5 |
                                ((uint32_t)r >> 3));
        }
    }
    ESP_LOGI(TAG, "splash painted to panel fb");
}
