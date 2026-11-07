/*
 * map_render_impl.h — C++ implementation of prepare_map_frame()
 *
 * Header-only. No dynamic allocation. Pure: reads only `fr`, writes only
 * `out`, no LVGL, no global UI state — safe on the comm thread.
 *
 * NOTE: writes DIRECTLY into `out` (no large local). A MapRenderFrame is
 * ~29 KB (30 lines × 120 pts); a stack local would overflow the ESP32 task
 * stack. The caller publishes `out` under its own mutex (see libre-moto.ino),
 * so writing in place is safe.
 */
#if !defined(LIBREMOTO_MAP_RENDER_H)
#error "include map_render.h, not this file directly"
#endif

extern "C" bool prepare_map_frame(const struct MapFrame *fr,
                                  struct MapRenderFrame *out) {
  if (fr == NULL || out == NULL) return false;

  /* Rotate the world so the vehicle heading points "up" (screen -y).
     heading unit = degrees × 10, 0° = north.  Screen: +x right, +y down. */
  double theta = (double)fr->heading * 0.1 * (3.14159265358979323846 / 180.0);
  double ct = cos(theta), st = sin(theta);

  memset(out, 0, sizeof *out);
  out->valid        = true;
  out->seq          = fr->seq;
  out->flags        = fr->flags;
  out->heading      = fr->heading;
  out->scale        = fr->scale;
  out->seg_count    = fr->seg_count;
  out->show_vehicle = (fr->flags & MAP_FLAG_VEHICLE) != 0;
  out->show_scale   = (fr->flags & MAP_FLAG_SCALE_BAR) != 0 && fr->scale > 0;
  if (out->show_scale) {
    out->scale_meters = (int)((100 * 100) / (int)fr->scale);
    if (out->scale_meters < 1) out->scale_meters = 1;
  }

  int used = 0;

  /* pool budget: 2 slots per road/branch (casing + fill), 1 per route/
     destination. Roads are prepared as CENTRE lines with a width — the
     renderer draws casing+fill in two passes (all casings below all
     fills), so crossing streets merge seamlessly instead of drawing
     a white cross of edge lines through each other. */
  int n_route = 0;
  for (int si = 0; si < fr->seg_count; si++)
    if (fr->segs[si].type == MAP_SEG_ROUTE) n_route++;
  int route_room = n_route + 2;                 /* +2: destination + margin */
  if (route_room > MAP_POOL) route_room = MAP_POOL;
  int road_room = (MAP_POOL - route_room) / 2;
  if (road_room < 0) road_room = 0;

  /* base[0] = rotated centre points (roads are centre lines now).
     File-local static: only the comm thread calls this, so no race.
     ~1.9 KB in .bss, off the task stack. */
  static double base[MAP_MAX_PTS * 2];

  /* ---- pass 1a: roads / branches → casing + fill centre lines -------
   *  TWO loops so the drawn order is: ALL casings, then ALL fills.
   *  (If casing+fill were interleaved per road, a later road's casing
   *  would stripe across an earlier road's fill at crossings — the
   *  exact artefact the two-pass scheme exists to avoid.) */
  for (int pass = 0; pass < 2; pass++) {
    int road_i = 0;
    for (int si = 0; si < fr->seg_count; si++) {
      const struct MapSeg *sg = &fr->segs[si];
      if (sg->type != MAP_SEG_ROAD && sg->type != MAP_SEG_BRANCH) continue;
      if (road_i >= road_room) continue;         /* pool exhausted — skip */
      road_i++;
      if (used >= MAP_POOL - 2 - n_route) break;

      int n = sg->npts;
      for (int i = 0; i < n; i++) {
        double x = sg->points[i * 2 + 0];
        double y = sg->points[i * 2 + 1];
        base[i * 2 + 0] =  x * ct + y * st + MAP_CX;
        base[i * 2 + 1] = -x * st + y * ct + MAP_CY;
      }

      /* fill width: the segment width byte is the road's fill width;
         casing = fill + 2 px so a dark outline stays visible around
         every road. Width floor 3 keeps the road body readable. */
      int fill_w = sg->width;
      if (fill_w < 3) fill_w = 3;

      struct MapRenderLine *ln = &out->lines[used];
      ln->type  = sg->type;
      ln->casing = (pass == 0);
      ln->width = (pass == 0) ? fill_w + 2 : fill_w;
      ln->npts  = n;
      for (int i = 0; i < n; i++) {
        ln->pts[i * 2 + 0] = (int32_t)lrint(base[i * 2 + 0]);
        ln->pts[i * 2 + 1] = (int32_t)lrint(base[i * 2 + 1]);
      }
      used++;
    }
  }

  /* ---- pass 1b: destination (accent, single line) ------------------ */
  for (int si = 0; si < fr->seg_count && used < MAP_POOL - n_route; si++) {
    const struct MapSeg *sg = &fr->segs[si];
    if (sg->type != MAP_SEG_DESTINATION) continue;
    struct MapRenderLine *ln = &out->lines[used];
    ln->type  = sg->type;
    ln->width = sg->width;
    ln->casing = false;
    ln->npts  = sg->npts;
    for (int i = 0; i < sg->npts; i++) {
      double x = sg->points[i * 2 + 0];
      double y = sg->points[i * 2 + 1];
      ln->pts[i * 2 + 0] = (int32_t)lrint( x * ct + y * st + MAP_CX);
      ln->pts[i * 2 + 1] = (int32_t)lrint(-x * st + y * ct + MAP_CY);
    }
    used++;
  }

  /* ---- pass 2: route (topmost, fat, rounded) ----------------------- */
  for (int si = 0; si < fr->seg_count && used < MAP_POOL; si++) {
    const struct MapSeg *sg = &fr->segs[si];
    if (sg->type != MAP_SEG_ROUTE) continue;
    struct MapRenderLine *ln = &out->lines[used];
    ln->type  = sg->type;
    ln->width = sg->width;
    ln->casing = false;
    ln->npts  = sg->npts;
    for (int i = 0; i < sg->npts; i++) {
      double x = sg->points[i * 2 + 0];
      double y = sg->points[i * 2 + 1];
      ln->pts[i * 2 + 0] = (int32_t)lrint( x * ct + y * st + MAP_CX);
      ln->pts[i * 2 + 1] = (int32_t)lrint(-x * st + y * ct + MAP_CY);
    }
    used++;
  }
  out->line_count = used;

  /* ---- destination dot marker (end point of destination segment) --- */
  out->marker_visible = false;
  for (int si = 0; si < fr->seg_count; si++) {
    const struct MapSeg *sg = &fr->segs[si];
    if (sg->type != MAP_SEG_DESTINATION) continue;
    int last = sg->npts - 1;
    double x = sg->points[last * 2 + 0];
    double y = sg->points[last * 2 + 1];
    double rx =  x * ct + y * st;
    double ry = -x * st + y * ct;
    out->marker_visible = true;
    out->marker_x = (int32_t)lrint(rx + MAP_CX);
    out->marker_y = (int32_t)lrint(ry + MAP_CY);
    break;
  }

  return true;   /* caller publishes `out` under its mutex */
}
