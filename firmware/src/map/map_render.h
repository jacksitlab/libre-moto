/*
 * map_render.h — Libre-Moto MapData geometry (rotation + road-edge offset)
 *
 * Header-only, no external dependencies: compiles on the host (unit tests,
 * -std=c++11) AND in the firmware (Arduino/ESP32).
 *
 * Splits the map pipeline into two stages so the heavy math never blocks the
 * LVGL thread:
 *
 *   1. GEOMETRY (this file) — pure math on a decoded MapFrame:
 *        world→screen rotation (heading up), road edge-line offset, integer
 *        rounding. No LVGL, no display objects, no global UI state.
 *        Runs on the COMM thread.
 *   2. RENDERING (libre-moto.ino) — copies the prepared lines into the LVGL
 *        line pool, sets styles, positions the marker/vehicle/scale bar.
 *        Runs on the DISPLAY thread (the one that calls lv_timer_handler).
 *
 * The hand-off is a fixed-size, allocation-free snapshot (MapRenderFrame)
 * that the display thread reads under a mutex. No dynamic allocation, no
 * LVGL types, so the geometry is unit-testable on the host.
 */

#ifndef LIBREMOTO_MAP_RENDER_H
#define LIBREMOTO_MAP_RENDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "map_frame.h"   /* MapFrame, MapSeg, MAP_MAX_PTS, MAP_SEG_*, MAP_FLAG_* */

#ifdef __cplusplus
extern "C" {
#endif

/* Segment pool size: how many LVGL lines the map screen can show at once.
 * (Lives here so the geometry and the renderer agree.) */
#define MAP_POOL 30

/* Screen centre (the world origin maps here). 480×480 panel → 240,240. */
#define MAP_CX 240.0
#define MAP_CY 240.0

/* One prepared line: screen-space points + the segment kind/width the
 * renderer needs to pick style (road/branch = thin white edge, destination
 * = accent, route = fat white). */
struct MapRenderLine {
  int      type;   /* MAP_SEG_* (drives colour + effective width) */
  int      width;  /* original segment width px (used for the route) */
  int      npts;   /* number of points */
  int32_t  pts[MAP_MAX_PTS * 2]; /* x0 y0 x1 y1 ... screen coords */
};

/* A fully prepared map frame, ready for the display thread to blit into the
 * LVGL pool. `lines[0..line_count-1]` are valid; the rest are unused. */
struct MapRenderFrame {
  bool     valid;        /* true once a frame has been prepared */
  uint8_t  seq;
  uint16_t flags;        /* MAP_FLAG_* (vehicle / scale-bar) */
  int      heading;      /* degrees × 10 (caption) */
  uint16_t scale;        /* px_per_meter × 100 (caption) */
  int      seg_count;    /* (caption) */
  bool     show_vehicle; /* MAP_FLAG_VEHICLE set */
  bool     show_scale;   /* MAP_FLAG_SCALE_BAR set && scale > 0 */
  int      scale_meters; /* metres represented by the 100 px scale bar */
  bool     marker_visible;
  int32_t  marker_x;     /* destination dot centre (screen coords) */
  int32_t  marker_y;
  int      line_count;
  struct MapRenderLine lines[MAP_POOL];
};

/* Compute the screen-space geometry for a decoded frame into `out`.
 *
 * Pure function: reads only `fr`, writes only `out`, touches no globals and
 * no LVGL. Safe to call from the comm thread. `out` is fully written (no
 * partial state) — the display thread can copy it under a mutex.
 *
 * Returns true on success (always, for a valid decoded frame). */
bool prepare_map_frame(const struct MapFrame *fr, struct MapRenderFrame *out);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

#include "map_render_impl.h"

#endif

#endif /* LIBREMOTO_MAP_RENDER_H */
