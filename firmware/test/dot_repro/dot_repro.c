/* Headless repro of the invisible BLE dot:
 * DIRECT mode + SINGLE buffer + small style-change invalidation,
 * exactly like the firmware (480x480 RGB565, one PSRAM buffer).
 *
 * Scenario:
 *   1. create home screen (dark bg) + a 22x22 dot at (229,309)
 *   2. lv_screen_load(home) + timer_handler  -> full redraw
 *   3. toggle the dot colour every 500 ms (like ble_blink_cb) + handler
 *   4. after each step: dump the PANEL buffer's dot region pixels
 *
 * If the dot vanishes after the first partial redraw -> reproduced;
 * we can then bisect inside LVGL right here, no hardware needed. */
#include "lvgl.h"
#include "src/display/lv_display_private.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define W 480
#define H 480

static uint16_t fb[W * H];          /* the DIRECT-mode render buffer   */
static uint16_t panel[W * H];       /* what the physical panel shows   */

static uint32_t tick_ms(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}

/* firmware-like flush: copy the flushed area into the panel buffer */
int g_flushes = 0;
static void my_flush(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
  g_flushes++;
  printf("  [FLUSH] area=(%d,%d..%d,%d)\n", (int)a->x1, (int)a->y1, (int)a->x2, (int)a->y2);
  const uint16_t *src = (const uint16_t *)px;
  /* what IS in the flushed region of the source buffer right now? */
  {
    uint16_t c = src[319 * W + 239];   /* dot centre in SCREEN coords */
    printf("  [SRC] px=%p fb_center_now=0x%04x\n", (void*)px, c);
  }
  /* DIRECT mode: px_map IS the full-screen buffer; the area is a window.
     Absolute indexing: src[(y1+row)*W + x1 + col] — NOT area-relative. */
  for (int row = 0; row <= a->y2 - a->y1; row++)
    memcpy(&panel[(a->y1 + row) * W + a->x1], &src[(a->y1 + row) * W + a->x1],
           (size_t)(a->x2 - a->x1 + 1) * 2);
  lv_display_flush_ready(d);
}

static lv_obj_t *dot;

static void dump_dot(const char *tag) {
  /* count non-background pixels in the dot region (229..250, 309..330) */
  int lit = 0; uint32_t csum = 0;
  for (int y = 309; y <= 330; y++)
    for (int x = 229; x <= 250; x++) {
      uint16_t c = panel[y * W + x];
      csum += c;
      if (c != 0x0842 && (c >> 5) > 0x18) lit++;   /* brighter than bg */
    }
  printf("%-28s panel dot region: lit=%3d csum=%u  center=0x%04x\n",
         tag, lit, csum, panel[319 * W + 239]);
}

static lv_obj_t *home;

static void blink_cb(lv_timer_t *t) {
  (void)t;
  static bool on = true;
  on = !on;
  lv_obj_set_style_bg_color(dot, on ? lv_color_hex(0x3FB950)
                                    : lv_color_hex(0x0A4A0F), 0);
  /* BISECT: dump the display's pending invalidation right after the
     style change — is the dot area actually queued for redraw? */
  lv_display_t *d = lv_display_get_default();
  lv_area_t ca; lv_obj_get_coords(dot, &ca);
  int found = -1;
  for (uint16_t i = 0; i < d->inv_p; i++) {
    if (d->inv_areas[i].x1 <= ca.x2 && d->inv_areas[i].x2 >= ca.x1 &&
        d->inv_areas[i].y1 <= ca.y2 && d->inv_areas[i].y2 >= ca.y1) { found = i; break; }
  }
  printf("  [BISECT] on=%d inv_p=%u dot_area_queued=%s\n",
         (int)on, (unsigned)d->inv_p, found >= 0 ? "YES" : "no");
}

int main(void) {
  lv_init();
  lv_tick_set_cb(tick_ms);

  lv_display_t *d = lv_display_create(W, H);
  lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(d, my_flush);
  lv_display_set_buffers(d, (uint8_t *)fb, NULL, sizeof fb,
                         LV_DISPLAY_RENDER_MODE_DIRECT);

  home = lv_obj_create(NULL);
  lv_obj_remove_style_all(home);
  lv_obj_set_size(home, W, H);
  lv_obj_set_style_bg_color(home, lv_color_hex(0x0D1117), 0);
  lv_obj_set_style_bg_opa(home, LV_OPA_COVER, 0);
  lv_obj_clear_flag(home, LV_OBJ_FLAG_SCROLLABLE);

  dot = lv_obj_create(home);
  lv_obj_remove_style_all(dot);
  lv_obj_set_size(dot, 22, 22);
  lv_obj_set_style_radius(dot, 11, 0);
  lv_obj_set_style_bg_color(dot, lv_color_hex(0xD29922), 0);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
  lv_obj_align(dot, LV_ALIGN_CENTER, 0, 80);   /* → (229,309) like the fw */
  lv_obj_update_layout(home);

  lv_timer_create(blink_cb, 500, NULL);

  /* 1. screen load -> full redraw */
  lv_screen_load(home);
  memset(panel, 0, sizeof panel);
  lv_timer_handler();
  dump_dot("after screen load");

  /* 2. partial redraws, like loop() calling handler every few ms */
  for (int i = 0; i < 6; i++) {
    lv_timer_handler();
    /* advance time past one blink period */
    struct timespec req = {0, 250 * 1000 * 1000};
    nanosleep(&req, NULL);
    lv_timer_handler();
    lv_display_t *dd = lv_display_get_default();
    char tag[96];
    snprintf(tag, sizeof tag, "cycle %d: flushes=%d inv_p=%u", i + 1, g_flushes, (unsigned)dd->inv_p);
    dump_dot(tag);
    printf("  [FB] fb center=0x%04x (dot region in the RENDER buffer)\n", fb[319 * W + 239]);
    g_flushes = 0;
  }
  return 0;
}
