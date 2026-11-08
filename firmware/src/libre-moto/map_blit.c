/* map_blit.c — full-frame software renderer (see map_blit.h).
 *
 * Fat Bresenham lines: for each segment we walk the axis with the
 * larger delta and stamp a (w×w) square brush at every step — with
 * the brush offset centred. Square brushes give the same look as
 * LVGL's non-rounded lines at a fraction of the cost; segment joints
 * overlap because consecutive segments re-stamp the shared endpoint.
 */
#include "../map/map_blit.h"
#include <string.h>

#define MB_W 480
#define MB_H 480

static uint16_t *g_buf = NULL;
static int g_dy = 0;   /* dead-reckoning world offset (px, +down) */

bool map_blit_init(uint16_t *framebuffer) {
  if (framebuffer == NULL) return false;
  g_buf = framebuffer;
  return true;
}

void map_blit_set_dy(int dy) { g_dy = dy; }

const uint16_t *map_blit_buffer(void) { return g_buf; }

/* non-const accessor for the LV-font text renderer */
uint16_t *mb_fb(void) { return g_buf; }

/* stamp a w×w square brush centred at (x,y) */
static inline void mb_brush(uint16_t *buf, int x, int y, int w, uint16_t col) {
  const int half = w / 2;
  const int x0 = x - half, y0 = y - half;
  if (x0 + w <= 0 || x0 >= MB_W || y0 + w <= 0 || y0 >= MB_H) return;
  const int xa = x0 < 0 ? 0 : x0;
  const int ya = y0 < 0 ? 0 : y0;
  const int xb = (x0 + w) > MB_W ? MB_W : (x0 + w);
  const int yb = (y0 + w) > MB_H ? MB_H : (y0 + w);
  for (int py = ya; py < yb; py++) {
    uint16_t *row = buf + (size_t)py * MB_W;
    for (int px = xa; px < xb; px++) row[px] = col;
  }
}

/* fat line from (x0,y0) to (x1,y1), brush w×w */
static void mb_line(uint16_t *buf, int x0, int y0, int x1, int y1,
                    int w, uint16_t col) {
  y0 += g_dy; y1 += g_dy;   /* dead-reckoning world offset */
  int dx = x1 - x0, dy = y1 - y0;
  int adx = dx < 0 ? -dx : dx;
  int ady = dy < 0 ? -dy : dy;
  int steps = adx > ady ? adx : ady;
  if (steps == 0) { mb_brush(buf, x0, y0, w, col); return; }
  for (int s = 0; s <= steps; s++) {
    const int x = x0 + (int)(((int64_t)dx * s) / steps);
    const int y = y0 + (int)(((int64_t)dy * s) / steps);
    mb_brush(buf, x, y, w, col);
  }
}

/* one polyline segment list in one colour */
static void mb_polyline(uint16_t *buf, const int32_t *pts, int npts,
                        int w, uint16_t col) {
  for (int i = 0; i + 1 < npts; i++) {
    mb_line(buf, pts[i * 2], pts[i * 2 + 1],
                 pts[i * 2 + 2], pts[i * 2 + 3], w, col);
  }
}

/* ARGB8888 (B,G,R,A bytes) → panel-order RGB565 (B and R fields
 * swapped for the ST7701 wiring). */
static inline uint16_t mb_argb_to_panel(uint8_t b, uint8_t g, uint8_t r) {
  return (uint16_t)(((uint32_t)b >> 3) << 11 |
                    ((uint32_t)g >> 2) << 5 |
                    ((uint32_t)r >> 3));
}

void map_blit_vehicle(const uint8_t *icon, int cx, int cy, int size) {
  if (g_buf == NULL || icon == NULL) return;
  const int S = size, half = S / 2;
  const int x0 = cx - half, y0 = cy - half;
  for (int y = 0; y < S; y++) {
    const int py = y0 + y;
    if (py < 0 || py >= MB_H) continue;
    for (int x = 0; x < S; x++) {
      const int px = x0 + x;
      if (px < 0 || px >= MB_W) continue;
      const uint8_t b = icon[(y * S + x) * 4 + 0];
      const uint8_t g = icon[(y * S + x) * 4 + 1];
      const uint8_t r = icon[(y * S + x) * 4 + 2];
      const uint8_t a = icon[(y * S + x) * 4 + 3];
      if (a == 0) continue;
      uint16_t *dst = &g_buf[(size_t)py * MB_W + px];
      if (a == 255) {
        *dst = mb_argb_to_panel(b, g, r);
      } else {
        /* alpha blend into the panel-order pixel */
        const uint16_t d = *dst;
        const uint16_t db = (d >> 11) & 0x1F, dg = (d >> 5) & 0x3F, dr = d & 0x1F;
        const uint32_t sb = (b >> 3), sg = (g >> 2), sr = (r >> 3);
        const uint32_t nb = (sb * a + db * (255 - a)) / 255;
        const uint32_t ng = (sg * a + dg * (255 - a)) / 255;
        const uint32_t nr = (sr * a + dr * (255 - a)) / 255;
        *dst = (uint16_t)((nb << 11) | (ng << 5) | nr);
      }
    }
  }
}

void map_blit_marker(int cx, int cy) {
  if (g_buf == NULL) return;
  const int R = 12;                      /* dot radius */
  const int RIM = 15;                    /* outer white rim radius */
  const uint16_t RIM_COL = 0xFFFF;       /* white */
  const uint16_t DOT = mb_argb_to_panel(0x50, 0xB9, 0x3F); /* C_ACCENT */
  for (int dy = -RIM; dy <= RIM; dy++) {
    for (int dx = -RIM; dx <= RIM; dx++) {
      const int d2 = dx * dx + dy * dy;
      if (d2 > RIM * RIM) continue;
      const int px = cx + dx, py = cy + dy;
      if (px < 0 || px >= MB_W || py < 0 || py >= MB_H) continue;
      g_buf[(size_t)py * MB_W + px] = (d2 <= R * R) ? DOT : RIM_COL;
    }
  }
}

/* ---- fixed overlay geometry (matches the old LVGL layout) -------- */
#define MB_BAND_H   84     /* info band height, flush to the bottom    */
#define MB_SIGN_CX 360    /* speed-limit sign centre (round display!)  */
#define MB_SIGN_CY 120
#define MB_SIGN_R   32     /* sign radius                              */
#define MB_SIGN_RING 7     /* red ring width                           */

void map_blit_band(void) {
  if (g_buf == NULL) return;
  const int y0 = MB_H - MB_BAND_H;
  /* LV_OPA_60 black over the map: out = map * (255-154)/255 ≈ *0.4 */
  for (int y = y0; y < MB_H; y++) {
    uint16_t *row = g_buf + (size_t)y * MB_W;
    for (int x = 0; x < MB_W; x++) {
      /* panel-order fields: B(5) G(6) R(5) — darken all equally */
      uint16_t c = row[x];
      const uint16_t b = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, r = c & 0x1F;
      row[x] = (uint16_t)(((b * 102) >> 8) << 11 |
                          ((g * 102) >> 8) << 5 |
                          ((r * 102) >> 8));
    }
  }
}

void map_blit_limit_sign(bool show) {
  if (g_buf == NULL || !show) return;
  const int16_t RED = mb_argb_to_panel(0x49, 0x51, 0xF8);  /* C_BAD red */
  const int16_t WHITE = 0xFFFF;
  const int outer = MB_SIGN_R, inner = MB_SIGN_R - MB_SIGN_RING;
  for (int dy = -outer; dy <= outer; dy++) {
    for (int dx = -outer; dx <= outer; dx++) {
      const int d2 = dx * dx + dy * dy;
      if (d2 > outer * outer) continue;
      const int px = MB_SIGN_CX + dx, py = MB_SIGN_CY + dy;
      if (px < 0 || px >= MB_W || py < 0 || py >= MB_H) continue;
      g_buf[(size_t)py * MB_W + px] = (d2 > inner * inner) ? RED : WHITE;
    }
  }
}

/* ---- 5x7 bitmap font: digits + uppercase + symbols ------------------
 * Each glyph: 7 rows × 5 bits (MSB left). Compact, readable at scale
 * 1-3 on the 480px round display. */
static const uint8_t MB_FONT[48][7] = {
  /* 0x20 ' ' */ {0,0,0,0,0,0,0},
  /* 0x21 '!' */ {0x04,0x04,0x04,0x04,0x04,0x00,0x04},
  /* 0x22 '"' */ {0x0A,0x0A,0x00,0x00,0x00,0x00,0x00},
  /* 0x2D '-' */ {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
  /* 0x2E '.' */ {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C},
  /* 0x2F '/' */ {0x00,0x01,0x02,0x04,0x08,0x10,0x00},
  /* 0x30 '0' */ {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
  /* 0x31 '1' */ {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
  /* 0x32 '2' */ {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F},
  /* 0x33 '3' */ {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
  /* 0x34 '4' */ {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
  /* 0x35 '5' */ {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
  /* 0x36 '6' */ {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},
  /* 0x37 '7' */ {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
  /* 0x38 '8' */ {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
  /* 0x39 '9' */ {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
  /* 0x3A ':' */ {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00},
  /* 0x6B 'k' K */ {0x10,0x10,0x12,0x14,0x18,0x14,0x12},
  /* 0x6D 'm' M */ {0x00,0x00,0x1E,0x15,0x15,0x15,0x15},
  /* 0x2C ',' */ {0x00,0x00,0x00,0x00,0x0C,0x04,0x08},
  /* 0x20+ — fill remaining slots lazily */
};
#define MB_FONT_BASE 0x30   /* '0' */
#define MB_FONT_NDIG 10

/* digits-only fast path + uppercase letters via table above */
static const uint8_t *mb_glyph(char c) {
  if (c >= '0' && c <= '9') return MB_FONT[c - '0'];
  switch (c) {
    case ' ': return MB_FONT[0];
    case '!': return MB_FONT[1];
    case '"': return MB_FONT[2];
    case '-': return MB_FONT[3];
    case '.': return MB_FONT[4];
    case '/': return MB_FONT[5];
    case ':': return MB_FONT[16];
    case 'K': return MB_FONT[17];
    case 'M': return MB_FONT[18];
    case ',': return MB_FONT[19];
    default:   return MB_FONT[0];
  }
}

void map_blit_text(const char *s, int x, int y, int scale, uint16_t col) {
  if (g_buf == NULL || s == NULL) return;
  int cx = x;
  for (const char *p = s; *p; p++) {
    const uint8_t *g = mb_glyph(*p);
    for (int gy = 0; gy < 7; gy++) {
      const uint8_t bits = g[gy];
      for (int gx = 0; gx < 5; gx++) {
        if (bits & (0x10 >> gx)) {
          for (int sy = 0; sy < scale; sy++) {
            for (int sx = 0; sx < scale; sx++) {
              const int px = cx + gx * scale + sx;
              const int py = y + gy * scale + sy;
              if (px >= 0 && px < MB_W && py >= 0 && py < MB_H)
                g_buf[(size_t)py * MB_W + px] = col;
            }
          }
        }
      }
    }
    cx += 6 * scale;   /* 5 px glyph + 1 px space */
  }
}

void map_blit_frame_dy(const struct MapRenderFrame *rf, int dy_world) {
  map_blit_set_dy(dy_world);
  map_blit_frame(rf);
  map_blit_set_dy(0);
}

void map_blit_frame(const struct MapRenderFrame *rf) {
  if (g_buf == NULL || rf == NULL) return;

  /* background: plain black — the round panel hides the corners */
  for (size_t i = 0; i < (size_t)MB_W * MB_H; i++) g_buf[i] = 0x0000;

  /* draw order identical to the old render_map_display():
   * 1. ALL road casings (white)
   * 2. ALL road fills (black body)
   * 3. destination line (accent)
   * 4. route (white, fat) — last, on top */
#ifdef MB_HOST_DEBUG
  printf("MBDBG line_count=%d g_buf=%p\n", rf->line_count, (void*)g_buf);
#endif
  for (int i = 0; i < rf->line_count && i < MAP_POOL; i++) {
    const struct MapRenderLine *ln = &rf->lines[i];
#ifdef MB_HOST_DEBUG
    printf("MBDBG line %d type=%d casing=%d npts=%d\n", i, ln->type, (int)ln->casing, ln->npts);
#endif
    if (ln->type != MAP_SEG_ROAD && ln->type != MAP_SEG_BRANCH) continue;
    if (!ln->casing) continue;
    mb_polyline(g_buf, ln->pts, ln->npts, ln->width, MB_COL_TEXT);
  }
  for (int i = 0; i < rf->line_count && i < MAP_POOL; i++) {
    const struct MapRenderLine *ln = &rf->lines[i];
    if (ln->type != MAP_SEG_ROAD && ln->type != MAP_SEG_BRANCH) continue;
    if (ln->casing) continue;
    mb_polyline(g_buf, ln->pts, ln->npts, ln->width, MB_COL_BG);
  }
  for (int i = 0; i < rf->line_count && i < MAP_POOL; i++) {
    const struct MapRenderLine *ln = &rf->lines[i];
    if (ln->type != MAP_SEG_DESTINATION) continue;
    mb_polyline(g_buf, ln->pts, ln->npts, 4, MB_COL_ACCENT);
  }
  for (int i = 0; i < rf->line_count && i < MAP_POOL; i++) {
    const struct MapRenderLine *ln = &rf->lines[i];
    if (ln->type != MAP_SEG_ROUTE) continue;
    mb_polyline(g_buf, ln->pts, ln->npts, ln->width, MB_COL_TEXT);
  }
}
