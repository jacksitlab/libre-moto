/*
 * test_map_frame.cpp — unit tests for map_frame.h (host, C++11)
 *
 * Build & run:
 *   g++ -std=c++11 -Wall -Wextra -Werror -O2 -I../src/map test_map_frame.cpp -o test_map_frame && ./test_map_frame
 *
 * Self-contained (no test framework). Exit code 0 = all pass.
 */
#include "map_frame.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

static int g_pass = 0;
static int g_fail = 0;

static bool T(bool cond, const char *name, int line) {
  if (cond) { ++g_pass; printf("  PASS  %s\n", name); }
  else      { ++g_fail; printf("  FAIL  %s   (line %d)\n", name, line); }
  return cond;
}
#define CHECK(cond, name) T((cond), (name), __LINE__)

/* --- little-endian frame builder ---------------------------------- */
static void put_u8(std::vector<uint8_t> &v, uint8_t x)   { v.push_back(x); }
static void put_u16(std::vector<uint8_t> &v, uint16_t x) { v.push_back(x & 0xFF); v.push_back(x >> 8); }
static void put_i16(std::vector<uint8_t> &v, int16_t x)  { put_u16(v, (uint16_t)x); }

struct pt { int16_t x, y; };

/* segment: first point absolute, rest delta-encoded (deltas must be i16-safe) */
static void put_seg(std::vector<uint8_t> &v, uint8_t type, uint8_t width,
                    const pt *pts, uint8_t n) {
  put_u8(v, type); put_u8(v, width); put_u8(v, n);
  for (int i = 0; i < n; i++) {
    int16_t dx = (i == 0) ? pts[0].x : (int16_t)((int32_t)pts[i].x - pts[i-1].x);
    int16_t dy = (i == 0) ? pts[0].y : (int16_t)((int32_t)pts[i].y - pts[i-1].y);
    put_i16(v, dx); put_i16(v, dy);
  }
}

static void put_hdr(std::vector<uint8_t> &v, uint16_t magic, uint8_t ver,
                    uint8_t seq, uint16_t flags, int16_t heading,
                    uint16_t scale, uint8_t seg_count) {
  put_u16(v, magic); put_u8(v, ver); put_u8(v, seq);
  put_u16(v, flags); put_i16(v, heading); put_u16(v, scale);
  put_u8(v, seg_count);
}

/* a route of n points along +y (down-screen), each step `step` px below center */
static void route(std::vector<pt> &v, int n, int step, int start) {
  v.clear();
  for (int i = 0; i < n; i++) v.push_back({ (int16_t)(start), (int16_t)(0 - i*step) });
}

int main() {
  std::vector<uint8_t> b;
  std::vector<pt> r;
  MapFrame f;

  /* --- 1: single route segment, 3 points (protocol §4 example) ---- */
  route(r, 3, 60, 0);   /* {0,0} {0,-60} {0,-120} */
  put_hdr(b, MAP_MAGIC, MAP_VER, 7, MAP_FLAG_ROTATE|MAP_FLAG_VEHICLE, 455, 400, 1);
  put_seg(b, MAP_SEG_ROUTE, 5, r.data(), 3);
  CHECK(parse_map_frame(b.data(), b.size(), &f), "1: parses (route, 3 pts)");
  CHECK(f.ver == MAP_VER, "1: ver");
  CHECK(f.seq == 7, "1: seq");
  CHECK((f.flags & MAP_FLAG_ROTATE) && (f.flags & MAP_FLAG_VEHICLE), "1: flags");
  CHECK(f.heading == 455, "1: heading (deg x10)");
  CHECK(f.scale == 400, "1: scale (px/m x100)");
  CHECK(f.seg_count == 1, "1: seg_count");
  CHECK(f.segs[0].type == MAP_SEG_ROUTE && f.segs[0].width == 5
        && f.segs[0].npts == 3, "1: seg header");
  CHECK(f.segs[0].points[0] == 0 && f.segs[0].points[1] == 0
       && f.segs[0].points[2] == 0 && f.segs[0].points[3] == -60
       && f.segs[0].points[4] == 0 && f.segs[0].points[5] == -120,
        "1: absolute decode of delta points");

  /* --- 2: mixed frame — route + road + destination ---------------- */
  b.clear();
  pt road[2] = { {-80, 0}, {80, 0} };
  pt dest[2] = { {0,-160}, {0,-200} };
  put_hdr(b, MAP_MAGIC, MAP_VER, 0, MAP_FLAG_SCALE_BAR, -120, 250, 3);
  put_seg(b, MAP_SEG_ROUTE, 6, r.data(), 3);
  put_seg(b, MAP_SEG_ROAD, 3, road, 2);
  put_seg(b, MAP_SEG_DESTINATION, 2, dest, 2);
  CHECK(parse_map_frame(b.data(), b.size(), &f), "2: parses (3 segs)");
  CHECK(f.seg_count == 3, "2: seg_count == 3");
  CHECK(f.segs[1].type == MAP_SEG_ROAD && f.segs[1].npts == 2, "2: road seg");
  CHECK(f.segs[2].type == MAP_SEG_DESTINATION, "2: destination seg");
  CHECK(f.segs[1].points[0] == -80 && f.segs[1].points[2] == 80, "2: road abs pts");
  CHECK(f.heading == -120, "2: negative heading (i16)");

  /* --- 3: extreme header values (all i16/u8 bounds) --------------- */
  b.clear();
  put_hdr(b, MAP_MAGIC, MAP_VER, 255, 0xFFFF, 32767, 65535, 1);
  pt mn[2] = {{0,0},{1,1}};
  put_seg(b, MAP_SEG_ROUTE, 1, mn, 2);                 /* type/width min */
  CHECK(parse_map_frame(b.data(), b.size(), &f), "3: max header parses");
  CHECK(f.seq == 255, "3: seq max 255");
  CHECK(f.flags == 0xFFFF, "3: all flag bits set");
  CHECK(f.heading == 32767, "3: heading i16 max");
  CHECK(f.scale == 65535, "3: scale u16 max");
  CHECK(f.segs[0].width == 1, "3: width min 1");
  CHECK(f.segs[0].points[0] == 0 && f.segs[0].points[2] == 1, "3: points decode");

  /* --- 3b: 120-point segment (protocol max) ----------------------- */
  b.clear();
  put_hdr(b, MAP_MAGIC, MAP_VER, 0, 0, 0, 0, 1);
  std::vector<pt> big(MAP_MAX_PTS);
  for (int i = 0; i < MAP_MAX_PTS; i++) big[i] = { (int16_t)(i*2), 0 }; /* step 2 */
  put_seg(b, MAP_SEG_ROUTE, 8, big.data(), MAP_MAX_PTS); /* width max 8 */
  CHECK(parse_map_frame(b.data(), b.size(), &f), "3b: 120-point segment parses");
  CHECK(f.segs[0].npts == MAP_MAX_PTS, "3b: 120 pts accepted");
  CHECK(f.segs[0].points[MAP_MAX_PTS*2-2] == (MAP_MAX_PTS-1)*2, "3b: last x point");

  /* --- 4: rejections ------------------------------------------------ */
  {
    std::vector<uint8_t> bad = b;
    bad[0] = 0x00;                                      /* corrupt magic   */
    CHECK(!parse_map_frame(bad.data(), bad.size(), &f), "4: bad magic rejected");
  }
  {
    b.clear();
    put_hdr(b, MAP_MAGIC, 2 /*unknown ver*/, 0, 0, 0, 0, 1);
    route(r, 3, 10, 0); put_seg(b, 1, 2, r.data(), 3);
    CHECK(!parse_map_frame(b.data(), b.size(), &f), "4: unknown ver rejected");
  }
  {
    b.clear();
    put_hdr(b, MAP_MAGIC, MAP_VER, 0, 0, 0, 0, 31);      /* seg_count > 30 */
    CHECK(!parse_map_frame(b.data(), b.size(), &f), "4: seg_count 31 rejected");
  }
  {
    b.clear();
    put_hdr(b, MAP_MAGIC, MAP_VER, 0, 0, 0, 0, 0);       /* seg_count = 0 */
    CHECK(!parse_map_frame(b.data(), b.size(), &f), "4: seg_count 0 rejected");
  }
  {
    b.clear();
    put_hdr(b, MAP_MAGIC, MAP_VER, 0, 0, 0, 0, 1);
    put_u8(b, 9 /*bad type*/); put_u8(b, 2); put_u8(b, 2);
    put_i16(b, 0); put_i16(b, 0); put_i16(b, 10); put_i16(b, 10);
    CHECK(!parse_map_frame(b.data(), b.size(), &f), "4: bad seg type rejected");
  }
  {
    b.clear();
    put_hdr(b, MAP_MAGIC, MAP_VER, 0, 0, 0, 0, 1);
    put_u8(b, 1); put_u8(b, 0 /*width 0*/); put_u8(b, 2);
    put_i16(b, 0); put_i16(b, 0); put_i16(b, 10); put_i16(b, 10);
    CHECK(!parse_map_frame(b.data(), b.size(), &f), "4: width 0 rejected");
  }
  {
    b.clear();
    put_hdr(b, MAP_MAGIC, MAP_VER, 0, 0, 0, 0, 1);
    put_u8(b, 1); put_u8(b, 9 /*width 9*/); put_u8(b, 2);
    put_i16(b, 0); put_i16(b, 0); put_i16(b, 10); put_i16(b, 10);
    CHECK(!parse_map_frame(b.data(), b.size(), &f), "4: width 9 rejected");
  }
  {
    b.clear();
    put_hdr(b, MAP_MAGIC, MAP_VER, 0, 0, 0, 0, 1);
    put_u8(b, 1); put_u8(b, 2); put_u8(b, 1 /*npts < 2*/);
    put_i16(b, 0); put_i16(b, 0);
    CHECK(!parse_map_frame(b.data(), b.size(), &f), "4: npts 1 rejected");
  }
  {
    b.clear();
    put_hdr(b, MAP_MAGIC, MAP_VER, 0, 0, 0, 0, 1);
    put_u8(b, 1); put_u8(b, 2); put_u8(b, 121 /*npts > 120*/);
    CHECK(!parse_map_frame(b.data(), b.size(), &f), "4: npts 121 rejected");
  }
  {
    /* truncated segment payload: promises 3 pts, ships 1.5 */
    b.clear();
    put_hdr(b, MAP_MAGIC, MAP_VER, 0, 0, 0, 0, 1);
    put_u8(b, 1); put_u8(b, 2); put_u8(b, 3);
    put_i16(b, 0); put_i16(b, 0); put_i16(b, 10);
    CHECK(!parse_map_frame(b.data(), b.size(), &f), "4: truncated payload rejected");
  }
  {
    /* trailing garbage → strict length check */
    b.clear();
    route(r, 2, 10, 0);
    put_hdr(b, MAP_MAGIC, MAP_VER, 0, 0, 0, 0, 1);
    put_seg(b, 1, 2, r.data(), 2);
    b.push_back(0xAB);
    CHECK(!parse_map_frame(b.data(), b.size(), &f), "4: trailing bytes rejected");
  }
  {
    uint8_t tiny[5] = { 1,2,3,4,5 };
    CHECK(!parse_map_frame(tiny, 5, &f), "4: 5-byte buffer rejected");
    CHECK(parse_map_frame(nullptr, 0, &f) == false, "4: nullptr rejected");
  }

  /* --- 5: out untouched on failure (contract) --------------------- */
  {
    MapFrame keep; memset(&keep, 0x55, sizeof keep);
    uint8_t junk[4] = { 0x4C, 0x4D, 0x02, 0x00 };       /* wrong order+short */
    parse_map_frame(junk, 4, &keep);
    bool untouched = (keep.ver == 0x55 && keep.seg_count == 0x55);
    CHECK(untouched, "5: out untouched on failure");
    CHECK(keep.segs[0].points[0] == (int16_t)0x5555, "5: out payload untouched");
  }

  /* --- 6: byte-exact 2 KB+ frame (12 segments, ~8 KB? ≤ 4 KB) ------ */
  {
    std::vector<uint8_t> bigf;
    put_hdr(bigf, MAP_MAGIC, MAP_VER, 1,
            MAP_FLAG_ROTATE|MAP_FLAG_SCALE_BAR|MAP_FLAG_VEHICLE, 900, 500, 12);
    for (int s = 0; s < 12; s++) {
      int n = 48;                        /* 12 * 48 pts * 4 B ≈ 2.3 KB */
      std::vector<pt> seg(n);
      for (int i = 0; i < n; i++) seg[i] = { (int16_t)(i*6 - 120), (int16_t)(-i*4) };
      put_seg(bigf, (uint8_t)(MAP_SEG_ROAD + (s % 3)),   /* types 2,3,4 */
              (uint8_t)(2 + s % 7),                        /* width 2..8  */
              seg.data(), (uint8_t)n);
    }
    CHECK(bigf.size() > 2000, "6: 12-segment frame > 2 KB");
    CHECK(bigf.size() <= 4096, "6: frame <= 4 KB ceiling");
    CHECK(parse_map_frame(bigf.data(), bigf.size(), &f), "6: 12-segment frame parses");
    CHECK(f.seg_count == 12, "6: 12 segs");
  }

  printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
