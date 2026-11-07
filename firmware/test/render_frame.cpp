/*
 * render_frame.cpp — END-TO-END display preview (host, C++11).
 *
 * Runs the REAL firmware pipeline on a frame.bin produced by the Kotlin
 * EndToEndPreviewTest:
 *
 *   parse_map_frame (map_frame_parse_impl.h — the exact ESP32 decoder)
 *   → prepare_map_frame (map_render_impl.h — the exact ESP32 geometry:
 *      heading rotation, road edge offset, draw order, scale bar metres)
 *   → PNG with the firmware's LVGL styles (libre-moto.ino):
 *        road/branch = 2 px white edge lines, dark middle
 *        route       = fat white, rounded
 *        destination = 4 px accent
 *        vehicle     = accent dot at centre
 *
 * What you see in the PNG is what the 480×480 panel shows.
 *
 * Build:
 *   g++ -std=c++11 -Wall -O2 -I../src/map render_frame.cpp -o render_frame
 * Run:
 *   ./render_frame <frame.bin> [out.png]
 */
#include "map_frame.h"
#include "map_render.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <fstream>
#include <zlib.h>

/* ---- minimal PNG writer (RGB8) --------------------------------------- */
static void write_png(const char *path, int w, int h, const std::vector<uint8_t> &rgb);

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s <frame.bin> [out.png]\n", argv[0]); return 1; }
  const char *binPath = argv[1];
  const char *pngPath = argc > 2 ? argv[2] : "frame-preview.png";

  std::ifstream f(binPath, std::ios::binary);
  if (!f) { fprintf(stderr, "kann %s nicht oeffnen\n", binPath); return 1; }
  std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
  printf("frame.bin: %zu Bytes\n", buf.size());

  /* ---- 1. the REAL firmware parser ---- */
  MapFrame fr;
  if (!parse_map_frame(buf.data(), buf.size(), &fr)) {
    fprintf(stderr, "PARSE-FEHLER: Firmware-Decoder verwirft den Frame!\n");
    return 2;
  }
  printf("Frame: seq=%u flags=0x%04X heading=%d scale=%u segs=%u\n",
         fr.seq, fr.flags, fr.heading, fr.scale, fr.seg_count);

  /* ---- 2. the REAL firmware geometry (rotation, edge offset, order) ---- */
  MapRenderFrame rf;
  if (!prepare_map_frame(&fr, &rf)) { fprintf(stderr, "prepare fehlgeschlagen\n"); return 3; }
  printf("Zeilen: %d (Roads zuerst, Route zuletzt — wie auf dem Display)\n", rf.line_count);
  for (int i = 0; i < rf.line_count; i++) {
    printf("  Zeile %d: type=%d width=%d npts=%d\n",
           i, rf.lines[i].type, rf.lines[i].width, rf.lines[i].npts);
  }
  if (rf.show_scale)
    printf("Massstab: 100 px = %d m\n", rf.scale_meters);

  /* ---- 3. draw with the firmware's LVGL styles ---- */
  const int W = 480, H = 480;
  std::vector<uint8_t> img(W * H * 3, 0);
  auto put = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (x < 0 || x >= W || y < 0 || y >= H) return;
    img[(y * W + x) * 3 + 0] = r; img[(y * W + x) * 3 + 1] = g; img[(y * W + x) * 3 + 2] = b;
  };
  auto stroke = [&](int x0, int y0, int x1, int y1, int wpx,
                   uint8_t r, uint8_t g, uint8_t b) {
    int steps = std::max(std::abs(x1 - x0), std::abs(y1 - y0)) * 2 + 1;
    int rad = wpx / 2;
    for (int s = 0; s <= steps; s++) {
      double t = (double)s / steps;
      int x = (int)(x0 + (x1 - x0) * t);
      int y = (int)(y0 + (y1 - y0) * t);
      for (int dy = -rad; dy <= rad; dy++)
        for (int dx = -rad; dx <= rad; dx++)
          if (dx * dx + dy * dy <= rad * rad) put(x + dx, y + dy, r, g, b);
    }
  };

  /* firmware palette (libre-moto.ino) */
  const uint8_t BG[3]    = {0x0D, 0x11, 0x17};
  const uint8_t WHITE[3] = {0xE6, 0xEE, 0xF3};
  const uint8_t ACCENT[3]= {0x3F, 0xB9, 0x50};
  for (int i = 0; i < W * H; i++) memcpy(&img[i * 3], BG, 3);

  /* draw order exactly like render_map_display(): ALL road casings
     (white), then ALL road fills (black body), then destination, route
     (white) last — "black line with white border", crossings join. */
  for (int i = 0; i < rf.line_count; i++) {
    const MapRenderLine *ln = &rf.lines[i];
    const uint8_t *col;
    int wpx;
    if (ln->type == MAP_SEG_ROAD || ln->type == MAP_SEG_BRANCH) {
      col = ln->casing ? WHITE : BG;
      wpx = ln->width;
    } else if (ln->type == MAP_SEG_DESTINATION) {
      col = ACCENT; wpx = 4;
    } else { /* route */
      col = WHITE; wpx = ln->width;
    }
    for (int p = 0; p + 1 < ln->npts; p++) {
      stroke(ln->pts[p * 2], ln->pts[p * 2 + 1],
             ln->pts[(p + 1) * 2], ln->pts[(p + 1) * 2 + 1], wpx, col[0], col[1], col[2]);
    }
  }
  /* vehicle dot */
  for (int dy = -6; dy <= 6; dy++)
    for (int dx = -6; dx <= 6; dx++)
      if (dx * dx + dy * dy <= 36) put(240 + dx, 320 + dy, ACCENT[0], ACCENT[1], ACCENT[2]);
  /* scale bar */
  if (rf.show_scale) {
    for (int x = 20; x < 120; x++) { put(x, 455, WHITE[0], WHITE[1], WHITE[2]);
                                     put(x, 458, WHITE[0], WHITE[1], WHITE[2]); }
  }

  write_png(pngPath, W, H, img);
  printf("PNG: %s\n", pngPath);
  return 0;
}

/* ---- PNG writer -------------------------------------------------------- */
static uint32_t crc32b(const uint8_t *d, size_t n) {
  uint32_t c = 0xFFFFFFFF;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320 & (-(c & 1)));
  }
  return c ^ 0xFFFFFFFF;
}

static void chunk(std::vector<uint8_t> &out, const char *type,
                  const std::vector<uint8_t> &data) {
  uint8_t len[4] = {(uint8_t)(data.size() >> 24), (uint8_t)(data.size() >> 16),
                    (uint8_t)(data.size() >> 8), (uint8_t)data.size()};
  out.insert(out.end(), len, len + 4);
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data.begin(), data.end());
  std::vector<uint8_t> crcIn(type, type + 4);
  crcIn.insert(crcIn.end(), data.begin(), data.end());
  uint32_t c = crc32b(crcIn.data(), crcIn.size());
  uint8_t crc[4] = {(uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
  out.insert(out.end(), crc, crc + 4);
}

static void write_png(const char *path, int w, int h, const std::vector<uint8_t> &rgb) {
  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<uint8_t> ihdr(13);
  ihdr[0] = w >> 24; ihdr[1] = w >> 16; ihdr[2] = w >> 8; ihdr[3] = (uint8_t)w;
  ihdr[4] = h >> 24; ihdr[5] = h >> 16; ihdr[6] = h >> 8; ihdr[7] = (uint8_t)h;
  ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  chunk(png, "IHDR", ihdr);

  /* raw scanlines with filter 0 */
  std::vector<uint8_t> raw(h * (1 + w * 3));
  size_t o = 0;
  for (int y = 0; y < h; y++) {
    raw[o++] = 0;
    memcpy(&raw[o], &rgb[y * w * 3], w * 3);
    o += w * 3;
  }
  uLongf comp = compressBound(raw.size());
  std::vector<uint8_t> idat(comp);
  compress2(idat.data(), &comp, raw.data(), raw.size(), 9);
  idat.resize(comp);
  chunk(png, "IDAT", idat);
  chunk(png, "IEND", {});
  std::ofstream out(path, std::ios::binary);
  out.write((const char *)png.data(), png.size());
}
