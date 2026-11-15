/* splash.c — boot splash painted straight into the panel framebuffer.
 *
 * Same approach as the sketch (LVGL must NOT render a full screen in
 * DIRECT mode during boot — it hangs): fill black + blit the I8 logo
 * (palette + indices), R/B-swapped to panel byte order.
 *
 * ANIMATION MODE (website intro variant A "assembly"): instead of the
 * static logo, the generated splash_anim masks are composited over
 * time — letters stagger in, the i-dot drops with a bounce, the speed
 * trail streaks in, swooshes fly in from the sides and the needle
 * settles. The panel scans the fb continuously, so every tick is
 * visible immediately; app_main calls the ticker at ~50 fps while the
 * rest of the boot proceeds in parallel. After the animation the screen
 * shows the static logo, pixel-identical to splash_paint().
 */

#include "splash.h"

#include <string.h>
#include "esp_heap_caps.h"
#include "config.h"
#include "esp_log.h"
#include "panel.h"
#include "ble_gatt.h"
#include "libremoto_logo.h"
#include "splash_anim.h"

static const char *TAG = "splash";

/* panel byte order is R/B-swapped RGB565 — swap a standard value */
static inline uint16_t to_panel(uint16_t rgb565) {
    uint16_t r = (rgb565 >> 11) & 0x1f, g = (rgb565 >> 5) & 0x3f, b = rgb565 & 0x1f;
    return (uint16_t)((b << 11) | (g << 5) | r);
}

/* website --ease: cubic-bezier(.22,.75,.25,1) — approximated by a
 * cubic ease-out, indistinguishable at 50 fps. */
static inline float ease_out(float t) {
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

/* Alpha-blend one A8-masked element into the panel fb at (sx+dx, sy+dy)
 * with a global alpha factor (0-255). All coordinates are clipped. */
static void blit_mask(uint16_t *fb, const uint8_t *mask, int w, int h,
                     int sx, int sy, uint16_t col, int alpha, int dx, int dy) {
    if (alpha <= 0) return;
    const int x0 = sx + dx, y0 = sy + dy;
    for (int y = 0; y < h; y++) {
        int fby = y0 + y;
        if (fby < 0 || fby >= SCREEN_H) continue;
        const uint8_t *mrow = mask + (size_t)y * w;
        uint16_t *dst = fb + (size_t)fby * SCREEN_W;
        for (int x = 0; x < w; x++) {
            int a = (int)mrow[x] * alpha / 255;
            if (a <= 4) continue;
            int fbx = x0 + x;
            if (fbx < 0 || fbx >= SCREEN_W) continue;
            if (a >= 250) { dst[fbx] = col; continue; }
            uint16_t bg = dst[fbx];
            uint16_t br = (bg >> 11) & 0x1f, bgc = (bg >> 5) & 0x3f, bb = bg & 0x1f;
            uint16_t cr = (col >> 11) & 0x1f, cg = (col >> 5) & 0x3f, cb = col & 0x1f;
            dst[fbx] = (uint16_t)(((br * (255 - a) + cr * a) >> 8) << 11 |
                                  (uint16_t)(((bgc * (255 - a) + cg * a) >> 8) << 5) |
                                  ((bb * (255 - a) + cb * a) >> 8));
        }
    }
}

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

/* --- animation mode -------------------------------------------------- */

static uint32_t s_anim_start_ms = 0;
static bool     s_anim_done = false;

/* Offscreen staging buffer: compose the frame HERE, then one memcpy to
 * the panel fb. Painting directly into the panel fb made the DMA scan
 * catch the intermediate state (cleared area before the masks are
 * blitted) — visible as flicker on every tick. The map renderer uses
 * the same offscreen-first pattern. Allocated once, freed never. */
static uint16_t *s_anim_fb = NULL;

static uint16_t *anim_fb(void) {
    if (!s_anim_fb) {
        /* prefer PSRAM (450 KB), fall back to internal */
        s_anim_fb = (uint16_t *)heap_caps_malloc(
            sizeof(uint16_t) * SCREEN_W * SCREEN_H, MALLOC_CAP_SPIRAM);
        if (!s_anim_fb) {
            s_anim_fb = (uint16_t *)heap_caps_malloc(
                sizeof(uint16_t) * SCREEN_W * SCREEN_H, MALLOC_CAP_8BIT);
        }
        if (s_anim_fb) {
            for (size_t i = 0; i < (size_t)SCREEN_W * SCREEN_H; i++)
                s_anim_fb[i] = 0x0000;
        }
    }
    return s_anim_fb;
}

void splash_anim_start(void) {
    s_anim_start_ms = lm_millis();
    s_anim_done = false;
    ESP_LOGI(TAG, "splash animation started");
}

bool splash_anim_done(void) { return s_anim_done; }

/* One animation step: clear the splash area, composite every element
 * at its current phase. Called at ~50 fps from the app_main loop while
 * the boot proceeds in parallel. */
void splash_anim_tick(uint16_t *panel_fb) {
    if (!panel_fb || s_anim_done) return;
    uint16_t *fb = anim_fb();
    if (!fb) {                 /* no staging buffer: fall back to static */
        s_anim_done = true;
        splash_paint(panel_fb);
        return;
    }

    uint32_t t = lm_millis() - s_anim_start_ms;

    const int lw = 480, lh = 188;
    const int x0 = (SCREEN_W - lw) / 2, y0 = (SCREEN_H - lh) / 2;

    /* clear the splash area in the OFFSCREEN buffer — the panel keeps
     * showing the previous frame until the single memcpy below */
    for (int y = 0; y < lh; y++) {
        uint16_t *dst = fb + (size_t)(y0 + y) * SCREEN_W + x0;
        for (int x = 0; x < lw; x++) dst[x] = 0x0000;
    }

    for (int i = 0; i < splash_elem_count; i++) {
        const splash_elem_t *e = &splash_elems[i];
        if ((int32_t)t < (int32_t)e->delay_ms) continue;
        float p = (float)(t - e->delay_ms) / (float)e->dur_ms;
        if (p > 1.0f) p = 1.0f;

        int alpha = 255, dx = 0, dy = 0;
        switch (e->anim) {
        case 0:  /* chIn: fade in while rising 8 px */
            alpha = (int)(ease_out(p) * 255);
            dy = (int)((1.0f - ease_out(p)) * 8.0f);
            break;
        case 1: {  /* dotIn: drops from 8 px above with a small bounce */
            if (p < 0.6f) {
                float q = p / 0.6f;
                dy = -(int)((1.0f - ease_out(q)) * 8.0f);
            } else if (p < 1.0f) {
                dy = (int)((p - 0.6f) * 2.0f);  /* overshoot ~0.8 px */
            }
            break;
        }
        case 2:  /* streakIn: from the right, 28 px */
            alpha = (int)(ease_out(p) * 255);
            dx = (int)((1.0f - ease_out(p)) * 28.0f);
            break;
        case 3:  /* fromLeft: 18 px */
            alpha = (int)(ease_out(p) * 255);
            dx = (int)((1.0f - ease_out(p)) * -18.0f);
            break;
        case 4:  /* fromRight: 18 px */
            alpha = (int)(ease_out(p) * 255);
            dx = (int)((1.0f - ease_out(p)) * 18.0f);
            break;
        case 5:  /* settle: fade in while drifting in from -12 deg
                    (rotation approximated as a small x drift) */
            alpha = (int)(ease_out(p) * 255);
            dx = (int)((1.0f - ease_out(p)) * 6.0f);
            break;
        }
        blit_mask(fb, e->mask, e->w, e->h,
                  x0 + e->x, y0 + e->y, to_panel(e->rgb565), alpha, dx, dy);
    }

    /* ONE atomic update: the panel scan never sees a half-painted frame */
    memcpy(panel_fb, fb, (size_t)SCREEN_W * SCREEN_H * 2);

    if (t >= (uint32_t)splash_anim_total_ms) {
        s_anim_done = true;
        /* final frame = the static logo, pixel-identical to splash_paint */
        splash_paint(panel_fb);
        ESP_LOGI(TAG, "splash animation done");
    }
}
