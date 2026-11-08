/* map_blit.h — full-frame software renderer for the map view.
 *
 * Replaces the LVGL lv_line pool: 44 lines × ~20 pts made the SW
 * renderer take ~150 ms per frame (measured: lvgl=220 ms total,
 * flush=70 ms). This renderer paints the SAME geometry with fat
 * Bresenham lines directly into one 480×480 RGB565 buffer — the
 * palette is identical to the old LVGL styles (casing+fill two-pass,
 * crossings join automatically by draw order).
 *
 * The buffer is handed to LVGL as one lv_image (single object, one
 * invalidation) — or blitted straight to the panel when the map
 * screen is active without overlays in the way.
 */
#ifndef MAP_BLIT_H
#define MAP_BLIT_H

#include <stdint.h>
#include <stdbool.h>
#include "map_render.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Palette (must match the old LVGL styles in libre-moto.ino). */
#define MB_COL_BG      0x0000   /* C_BG   dark navy  (roads' fill)   */
#define MB_COL_TEXT    0xFFFF   /* C_TEXT white-ish (casings, route) */
#define MB_COL_ACCENT  0x053F   /* C_ACCENT green (destination)      */

/* Initialize with a caller-provided 480×480 RGB565 buffer (PSRAM ok).
 * Returns false if the buffer is NULL. */
bool map_blit_init(uint16_t *framebuffer);

/* Paint one prepared frame into the buffer (thread: display task).
 * Same draw order as the old render_map_display: casings → fills →
 * destination → route. Background is cleared to black (the round
 * display hides the corners anyway).
 *
 * dy_world: vertical world offset in px for DEAD RECKONING — the
 * vehicle moves forward between two real frames, so the whole world
 * (streets + route) is painted dy px further down. 0 = exact frame. */
void map_blit_frame_dy(const struct MapRenderFrame *rf, int dy_world);

/* Backwards-compatible exact frame paint. */
void map_blit_frame(const struct MapRenderFrame *rf);

/* The rendered buffer (480*480 RGB565) — for the flush path. */
const uint16_t *map_blit_buffer(void);

/* Paint the vehicle marker icon (64×64 ARGB8888, LVGL byte order
 * B,G,R,A per pixel) centred at (cx, cy) with alpha blending over the
 * painted map. Call after map_blit_frame. */
void map_blit_vehicle(const uint8_t *argb8888, int cx, int cy, int size);

/* Paint the destination marker (accent dot with white rim) centred at
 * (cx, cy). Call after map_blit_frame. */
void map_blit_marker(int cx, int cy);

/* Paint the bottom info band (full width, 84 px, semi-transparent black
 * over the map — Beeline style) and the speed-limit sign (white circle,
 * red ring) at the fixed round-display-safe position. Call after
 * map_blit_frame (before the vehicle marker). */
void map_blit_band(void);
void map_blit_limit_sign(bool show);

/* 5x7 bitmap text (digits, letters, a few symbols) rendered into the
 * frame — replaces the LVGL labels on the map screen (their regions
 * raced the panel scan and flickered as pixel streaks). */
void map_blit_text(const char *s, int x, int y, int scale, uint16_t col);

/* Render text with a REAL LVGL font (e.g. montserrat_34) into the
 * frame — anti-aliased (4 bpp glyphs), same look as the old labels.
 * `font` is an lv_font_t*; requires LVGL headers (map_blit_lv.c). */
void map_blit_text_lv(void *font, const char *s, int x, int y, uint16_t col);

#ifdef __cplusplus
}
#endif
#endif
