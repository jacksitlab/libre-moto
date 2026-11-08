/* test_map_blit.cpp — host test for the full-frame renderer.
 * Renders a synthetic frame (crossing roads + route) and checks the
 * pixel profile: casing white, fill black, route white on top. */
#include "map_blit.h"
/* map_blit.c compiled as C++ here: map_blit.h -> map_render.h ->
   map_render_impl.h is a header-only C++ implementation; including it
   from two TUs double-defines. We pull map_blit.c in via #include. */
#include "../src/libre-moto/map_blit.c"
#include <cstdio>
#include <cstring>
#include <cstring>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) do { \
  if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

int main() {
  static uint16_t buf[480 * 480];
  CHECK(map_blit_init(buf), "init");
  CHECK(!map_blit_init(NULL), "null buffer rejected");

  struct MapRenderFrame rf = {};
  rf.line_count = 5;
  /* two crossing roads: horizontal + vertical through the centre */
  auto setline = [&](int i, int type, int width, bool casing, int npts,
                     const int32_t *p) {
    rf.lines[i].type = type; rf.lines[i].width = width;
    rf.lines[i].casing = casing; rf.lines[i].npts = npts;
    memcpy(rf.lines[i].pts, p, npts * 2 * sizeof(int32_t));
  };
  int32_t h_casing[] = {100,240, 380,240};
  int32_t h_fill[]   = {100,240, 380,240};
  int32_t v_casing[] = {240,100, 240,380};
  int32_t v_fill[]   = {240,100, 240,380};
  int32_t route[]    = {240,380, 240,100};
  setline(0, MAP_SEG_ROAD,  11, true,  2, h_casing);
  setline(1, MAP_SEG_ROAD,   9, false, 2, h_fill);
  setline(2, MAP_SEG_ROAD,  11, true,  2, v_casing);
  setline(3, MAP_SEG_ROAD,   9, false, 2, v_fill);
  setline(4, MAP_SEG_ROUTE,  5, false, 2, route);

  map_blit_frame(&rf);

  /* background away from the roads */
  CHECK(buf[10 * 480 + 10] == 0x0000, "background black");
  /* horizontal road: casing white at the outer edge, fill black inside.
     The road is 9 px wide centred on y=240: casing rows 236..244, fill
     rows 238..242 → at x=150 (away from the crossing and the route):
     y=236 white (casing), y=240 black (fill) — BUT the route runs
     vertically at x=240, so x=150 shows the pure road profile. */
  CHECK(buf[235 * 480 + 150] == 0xFFFF, "h-road casing painted (white)");
  CHECK(buf[240 * 480 + 150] == 0x0000, "h-road fill painted (black)");
  /* casing must be brighter than fill: casing is 0xFFFF, fill is 0x0000.
     Verify the casing row is exactly white: */
  CHECK(buf[245 * 480 + 150] == 0xFFFF, "casing is white (both edges)");
  /* at the crossing (240,240) the route (white, drawn last) wins */
  CHECK(buf[240 * 480 + 240] == 0xFFFF, "route on top at the crossing");
  /* vertical road casing at y=150, x=236 (left of the route at x=240) */
  CHECK(buf[150 * 480 + 235] == 0xFFFF, "v-road casing white");
  /* route column: x=240, y=150 must be white (route width 5 → 238..242) */
  CHECK(buf[150 * 480 + 240] == 0xFFFF, "route painted white");

  printf(failures ? "FAILED: %d\n" : "all passed (%d failures)\n", failures);
  return failures ? 1 : 0;
}
