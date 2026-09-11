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

  /* pool budget: 1 slot per route/destination, 2 per road/branch */
  int n_route = 0;
  for (int si = 0; si < fr->seg_count; si++)
    if (fr->segs[si].type == MAP_SEG_ROUTE) n_route++;
  int route_room = n_route + 2;                 /* +2: destination + margin */
  if (route_room > MAP_POOL) route_room = MAP_POOL;
  int road_room = (MAP_POOL - route_room) / 2;
  if (road_room < 0) road_room = 0;

  /* base[0] = rotated centre points, base[1] = offset edge points.
     File-local static: only the comm thread calls this, so no race.
     ~3.8 KB in .bss, off the task stack. */
  static double base[2][MAP_MAX_PTS * 2];
  int road_i = 0;

  /* ---- pass 1a: roads / branches → 2 thin edge lines each ---------- */
  for (int si = 0; si < fr->seg_count; si++) {
    const struct MapSeg *sg = &fr->segs[si];
    if (sg->type != MAP_SEG_ROAD && sg->type != MAP_SEG_BRANCH) continue;
    if (road_i >= road_room) continue;         /* pool exhausted — skip */
    road_i++;

    int n = sg->npts;
    for (int i = 0; i < n; i++) {
      double x = sg->points[i * 2 + 0];
      double y = sg->points[i * 2 + 1];
      base[0][i * 2 + 0] =  x * ct + y * st + MAP_CX;
      base[0][i * 2 + 1] = -x * st + y * ct + MAP_CY;
    }

    double half = sg->width * 0.5;
    if (half < 1.5) half = 1.5;

    for (int e = 0; e < 2 && used < MAP_POOL - 2 - n_route; e++) {
      double sign = (e == 0) ? 1.0 : -1.0;
      /* per-vertex offset along the average of neighbouring normals so
         corners don't spike */
      for (int i = 0; i < n; i++) {
        double x = base[0][i * 2], y = base[0][i * 2 + 1];
        double nx = 0, ny = 0;
        if (n > 1) {
          if (i < n - 1) {
            double dx = base[0][(i + 1) * 2] - x, dy = base[0][(i + 1) * 2 + 1] - y;
            double L = sqrt(dx * dx + dy * dy);
            if (L > 0.0001) { nx += -dy / L; ny += dx / L; }
          }
          if (i > 0) {
            double dx = x - base[0][(i - 1) * 2], dy = y - base[0][(i - 1) * 2 + 1];
            double L = sqrt(dx * dx + dy * dy);
            if (L > 0.0001) { nx += -dy / L; ny += dx / L; }
          }
        }
        double L = sqrt(nx * nx + ny * ny);
        if (L < 0.0001) { nx = 0; ny = 1; } else { nx /= L; ny /= L; }
        double off = half * sign;
        base[1][i * 2 + 0] = x + nx * off;
        base[1][i * 2 + 1] = y + ny * off;
      }
      struct MapRenderLine *ln = &out->lines[used];
      ln->type  = sg->type;
      ln->width = sg->width;
      ln->npts  = n;
      for (int i = 0; i < n; i++) {
        ln->pts[i * 2 + 0] = (int32_t)lrint(base[1][i * 2 + 0]);
        ln->pts[i * 2 + 1] = (int32_t)lrint(base[1][i * 2 + 1]);
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
    ln->npts  = sg->npts;
    for (int i = 0; i < sg->npts; i++) {
      double x = sg->points[i * 2 + 0];
      double y = sg->points[i * 2 + 1];
      ln->pts[i * 2 + 0] = (int32_t)lrint( x * ct + y * st + MAP_CX);
      ln->pts[i * 2 + 1] = (int32_t)lrint(-x * st + y * ct + MAP_CY);
    }
    used++;
  }

  /* ---- pass 2: route (topmost, fat white, rounded) ----------------- */
  for (int si = 0; si < fr->seg_count && used < MAP_POOL; si++) {
    const struct MapSeg *sg = &fr->segs[si];
    if (sg->type != MAP_SEG_ROUTE) continue;
    struct MapRenderLine *ln = &out->lines[used];
    ln->type  = sg->type;
    ln->width = sg->width;
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
