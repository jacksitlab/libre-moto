/* map_blit_lv.c — LVGL-font text rendering into the map frame.
 * Uses the font's glyph bitmaps (4 bpp anti-aliased) directly, so the
 * painted text looks EXACTLY like the old LVGL labels — but without
 * any LVGL draw pipeline involvement (no racing the panel scan). */
#include "lvgl.h"
#include "../map/map_blit.h"
#include <string.h>

/* forward: the frame buffer lives in map_blit.c */
extern uint16_t *mb_fb(void);

void map_blit_text_lv(void *font_p, const char *s, int x, int y, uint16_t col) {
  const lv_font_t *font = (const lv_font_t *)font_p;
  uint16_t *buf = mb_fb();
  if (!font || !s || !buf) return;
  const int32_t W = 480, H = 480;

  /* static A8 draw buffer: the OFFICIAL conversion path. The raw 4 bpp
     bitstream has no per-line alignment (stride=0 in the font dsc) —
     hand-unpacking it misrendered wide glyphs ('2','0'). LVGL's
     get_glyph_bitmap(draw_buf) converts each glyph into a clean A8
     mask; we just blend it. 64x64 covers montserrat_40 glyphs. */
  static uint8_t glyph_px[64 * 64] __attribute__((aligned(16)));
  static lv_draw_buf_t gdbuf;

  int cx = x;
  for (const char *p = s; *p; p++) {
    lv_font_glyph_dsc_t dsc;
    if (!lv_font_get_glyph_dsc(font, &dsc, (uint32_t)*p, 0)) {
      cx += font->line_height / 3;
      continue;
    }
    const int bw = dsc.box_w, bh = dsc.box_h;
    if (bw <= 0 || bh <= 0 || bw > 64 || bh > 64) { cx += dsc.adv_w; continue; }
    if (lv_draw_buf_init(&gdbuf, bw, bh, LV_COLOR_FORMAT_A8, 0,
                         glyph_px, sizeof glyph_px) != LV_RESULT_OK) {
      cx += dsc.adv_w;
      continue;
    }
    const void *bmp = font->get_glyph_bitmap((lv_font_glyph_dsc_t *)&dsc, &gdbuf);
    if (bmp == NULL) { cx += dsc.adv_w; continue; }
    /* get_glyph_bitmap returns the lv_draw_buf_t HEADER (verified
       headless: pointer sits 48 bytes = sizeof(lv_draw_buf_t) before
       our data) — the A8 pixels are in ->data. */
    const lv_draw_buf_t *gbuf = (const lv_draw_buf_t *)bmp;
    const uint8_t *a8 = (const uint8_t *)gbuf->data;
    const int stride = (int)gbuf->header.stride ? (int)gbuf->header.stride : bw;
    for (int gy = 0; gy < bh; gy++) {
      for (int gx = 0; gx < bw; gx++) {
        const int alpha = a8[(size_t)gy * stride + gx];   /* 0..255 */
        if (alpha == 0) continue;
        const int px = cx + dsc.ofs_x + gx;
        /* LVGL's own formula (lv_draw_label.c:626): the glyph's TOP is
           y + (line_height - base_line) - box_h - ofs_y measured from
           the pen origin; base_line counts FROM THE BOTTOM of the line. */
        const int g_top = y + (font->line_height - font->base_line)
                            - dsc.box_h - dsc.ofs_y;
        const int py = g_top + gy;
        if (px < 0 || px >= W || py < 0 || py >= H) continue;
        uint16_t *dst = &buf[(size_t)py * W + px];
        const uint16_t d = *dst;
        const int cb = (col >> 11) & 0x1F, cg = (col >> 5) & 0x3F, cr_ = col & 0x1F;
        const int db = (d >> 11) & 0x1F, dg = (d >> 5) & 0x3F, dr_ = d & 0x1F;
        const int A = alpha, nA = 255 - alpha;
        *dst = (uint16_t)((((cb * A + db * nA) / 255) << 11) |
                          (((cg * A + dg * nA) / 255) << 5) |
                          (((cr_ * A + dr_ * nA) / 255)));
      }
    }
    cx += dsc.adv_w;   /* already in px */
  }
}
