/*
 * test_map_render.cpp — unit tests for map_render.h (host, C++11)
 *
 * Build & run:
 *   g++ -std=c++11 -Wall -Wextra -Werror -O2 -I../src/map test_map_render.cpp -o test_map_render && ./test_map_render
 *
 * Self-contained (no test framework). Exit code 0 = all pass.
 *
 * Verifies the pure geometry stage (rotation + road-edge offset + marker)
 * that runs on the comm thread, independent of LVGL.
 */
#include "map_render.h"
#include <cstdio>
#include <cstring>
#include <cmath>

static int g_pass = 0;
static int g_fail = 0;

static bool T(bool cond, const char *name, int line) {
  if (cond) { ++g_pass; printf("  PASS  %s\n", name); }
  else      { ++g_fail; printf("  FAIL  %s   (line %d)\n", name, line); }
  return cond;
}
#define CHECK(cond, name) T((cond), (name), __LINE__)

/* |a-b| <= eps */
static bool near32(int32_t a, int32_t b, int eps) {
  int32_t d = a - b;
  if (d < 0) d = -d;
  return d <= eps;
}

/* Build a decoded frame: one straight road (x -100..100 at y=0, width 12)
 * + one straight route (y 0..-200, width 10). Points are ABSOLUTE (as the
 * parser publishes them). */
static MapFrame make_road_route_frame(uint16_t flags, int16_t heading) {
  MapFrame fr;
  memset(&fr, 0, sizeof fr);
  fr.ver = 1;
  fr.seq = 7;
  fr.flags = flags;
  fr.heading = heading;
  fr.scale = 20;   /* 0.2 px/m → 100 px bar = 500 m */
  fr.seg_count = 2;

  fr.segs[0].type = MAP_SEG_ROAD;
  fr.segs[0].width = 12;
  fr.segs[0].npts = 2;
  fr.segs[0].points[0] = -100; fr.segs[0].points[1] = 0;
  fr.segs[0].points[2] = 100;  fr.segs[0].points[3] = 0;

  fr.segs[1].type = MAP_SEG_ROUTE;
  fr.segs[1].width = 10;
  fr.segs[1].npts = 3;
  fr.segs[1].points[0] = 0;   fr.segs[1].points[1] = 0;
  fr.segs[1].points[2] = 0;   fr.segs[1].points[3] = -100;
  fr.segs[1].points[4] = 0;   fr.segs[1].points[5] = -200;
  return fr;
}

int main() {
  MapRenderFrame rf;
  memset(&rf, 0, sizeof rf);

  /* ---- heading 0°: world = screen (centred) ----------------------- */
  {
    MapFrame fr = make_road_route_frame(MAP_FLAG_ROTATE | MAP_FLAG_VEHICLE | MAP_FLAG_SCALE_BAR, 0);
    CHECK(prepare_map_frame(&fr, &rf), "prepare returns true");
    CHECK(rf.valid, "valid set");
    CHECK(rf.seq == 7, "seq copied");
    CHECK(rf.show_vehicle, "vehicle flag");
    CHECK(rf.show_scale, "scale flag");
    CHECK(rf.scale_meters == 500, "scale 20 → 500 m");
    CHECK(rf.line_count == 3, "1 road → 2 edge lines + 1 route = 3");

    /* road edges: centre line y=240, half=6 → edges at y=246 (e=0, +n)
       and y=234 (e=1, −n). Normal of a horizontal line is (0,±1). */
    CHECK(rf.lines[0].type == MAP_SEG_ROAD, "line0 is road");
    CHECK(rf.lines[0].npts == 2, "line0 has 2 pts");
    CHECK(near32(rf.lines[0].pts[0], 140, 1) && near32(rf.lines[0].pts[1], 246, 1),
          "road edge A start (140,246)");
    CHECK(near32(rf.lines[0].pts[2], 340, 1) && near32(rf.lines[0].pts[3], 246, 1),
          "road edge A end (340,246)");
    CHECK(near32(rf.lines[1].pts[0], 140, 1) && near32(rf.lines[1].pts[1], 234, 1),
          "road edge B start (140,234)");
    CHECK(near32(rf.lines[1].pts[2], 340, 1) && near32(rf.lines[1].pts[3], 234, 1),
          "road edge B end (340,234)");

    /* route: x=240, y from 240 up to 40 */
    CHECK(rf.lines[2].type == MAP_SEG_ROUTE, "line2 is route");
    CHECK(rf.lines[2].width == 10, "route width kept");
    CHECK(near32(rf.lines[2].pts[0], 240, 1) && near32(rf.lines[2].pts[1], 240, 1),
          "route start at centre");
    CHECK(near32(rf.lines[2].pts[4], 240, 1) && near32(rf.lines[2].pts[5], 40, 1),
          "route end (240,40)");

    CHECK(!rf.marker_visible, "no destination → no marker");
  }

  /* ---- heading 90° (east): world +x rotates to screen -y (up) ------ */
  {
    MapFrame fr = make_road_route_frame(MAP_FLAG_ROTATE, 900);
    CHECK(prepare_map_frame(&fr, &rf), "prepare (90°) returns true");
    /* route point (0,-200) = 200 m north; heading east → north is left
       of screen: screen (-200, 0) + centre = (40, 240) */
    CHECK(near32(rf.lines[2].pts[4], 40, 1) && near32(rf.lines[2].pts[5], 240, 1),
          "route end rotated to (40,240) at heading 90°");
    /* road point (100, 0) = 100 m east = straight ahead → screen up:
       centre (240,140). The centerline is now vertical, so its two edges
       are at x=246 (e=0, +n) and x=234 (e=1, −n), same y. */
    CHECK(near32(rf.lines[0].pts[2], 246, 1) && near32(rf.lines[0].pts[3], 140, 1),
          "road edge A end (246,140) at heading 90°");
    CHECK(near32(rf.lines[1].pts[2], 234, 1) && near32(rf.lines[1].pts[3], 140, 1),
          "road edge B end (234,140) at heading 90°");
  }

  /* ---- heading 180° (south): world -y (north) → screen down -------- */
  {
    MapFrame fr = make_road_route_frame(MAP_FLAG_ROTATE, 1800);
    CHECK(prepare_map_frame(&fr, &rf), "prepare (180°) returns true");
    /* route end (0,-200): north → behind → screen (240, 440) */
    CHECK(near32(rf.lines[2].pts[4], 240, 1) && near32(rf.lines[2].pts[5], 440, 1),
          "route end rotated to (240,440) at heading 180°");
  }

  /* ---- destination marker ------------------------------------------ */
  {
    MapFrame fr = make_road_route_frame(MAP_FLAG_ROTATE, 0);
    fr.seg_count = 3;
    fr.segs[2].type = MAP_SEG_DESTINATION;
    fr.segs[2].width = 3;
    fr.segs[2].npts = 2;
    fr.segs[2].points[0] = 0;   fr.segs[2].points[1] = 0;
    fr.segs[2].points[2] = 0;   fr.segs[2].points[3] = -160;
    CHECK(prepare_map_frame(&fr, &rf), "prepare (dest) returns true");
    CHECK(rf.marker_visible, "marker visible");
    CHECK(near32(rf.marker_x, 240, 1) && near32(rf.marker_y, 80, 1),
          "marker at (240,80)");
    CHECK(rf.line_count == 4, "road(2) + dest(1) + route(1) = 4 lines");
    /* destination line is accent-coloured by the renderer; check order:
       roads first, then destination, then route on top. */
    CHECK(rf.lines[2].type == MAP_SEG_DESTINATION, "dest line before route");
    CHECK(rf.lines[3].type == MAP_SEG_ROUTE, "route last (top z-order)");
  }

  /* ---- pool budget: many roads must not overflow ------------------- */
  {
    MapFrame fr;
    memset(&fr, 0, sizeof fr);
    fr.ver = 1; fr.seq = 1; fr.flags = MAP_FLAG_ROTATE;
    fr.scale = 20;
    fr.seg_count = MAP_MAX_SEGS;
    for (int i = 0; i < MAP_MAX_SEGS; i++) {
      fr.segs[i].type = MAP_SEG_ROAD;
      fr.segs[i].width = 12;
      fr.segs[i].npts = 2;
      fr.segs[i].points[0] = -50; fr.segs[i].points[1] = -10 * i;
      fr.segs[i].points[2] = 50;  fr.segs[i].points[3] = -10 * i;
    }
    CHECK(prepare_map_frame(&fr, &rf), "prepare (30 roads) returns true");
    CHECK(rf.line_count <= MAP_POOL, "line_count within pool");
    /* 30 roads but road_room=(30-2)/2=14 → 14 roads × 2 edges = 28 lines
       (capped by the route/destination reservation). */
    CHECK(rf.line_count == 28, "pool capped at 28 edge lines for 30 roads");
  }

  /* ---- no rotation flag: geometry still centred (rotation is always
       * applied — the flag only tells the phone the frame is rotated;
       * the device always renders heading-up). ----------------------- */
  {
    MapFrame fr = make_road_route_frame(0, 0);
    CHECK(prepare_map_frame(&fr, &rf), "prepare (no flags) returns true");
    CHECK(!rf.show_vehicle, "no vehicle flag → hidden");
    CHECK(!rf.show_scale, "no scale flag → hidden");
  }

  /* ---- null safety -------------------------------------------------- */
  {
    MapFrame fr = make_road_route_frame(MAP_FLAG_ROTATE, 0);
    CHECK(!prepare_map_frame(NULL, &rf), "NULL frame rejected");
    CHECK(!prepare_map_frame(&fr, NULL), "NULL out rejected");
  }

  printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
