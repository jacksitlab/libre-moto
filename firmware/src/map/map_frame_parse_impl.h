/*
 * map_frame_parse_impl.h — C++ implementation of parse_map_frame()
 *
 * Header-only. No dynamic allocation. Bounds-checked end to end:
 * a malformed frame can never write past `out`.
 */

#if !defined(LIBREMOTO_MAP_FRAME_H)
#error "include map_frame.h, not this file directly"
#endif

#include <stdint.h>

namespace {

/* little-endian reads */
static inline int16_t  rd_i16(const uint8_t *p) {
  return (int16_t)(uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static inline uint16_t rd_u16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* clamp an (unbounded) coordinate accumulator into int16 range —
 * malformed frames may accumulate past the signed range; the renderer
 * only needs something finite on screen. */
static inline int16_t clamp_i16(int32_t v) {
  if (v > 32767)  return 32767;
  if (v < -32768) return -32768;
  return (int16_t)v;
}

}  /* namespace */

extern "C" bool parse_map_frame(const uint8_t *buf, size_t len, struct MapFrame *out) {
  if (buf == NULL || out == NULL) return false;

  /* --- fixed header (12 bytes) ------------------------------------ */
  if (len < MAP_FRAME_HEADER_SIZE) return false;

  if (rd_u16(buf + 0) != MAP_MAGIC) return false;
  uint8_t  ver       = buf[2];
  uint8_t  seq       = buf[3];
  uint16_t flags     = rd_u16(buf + 4);
  int16_t  heading   = rd_i16(buf + 6);
  uint16_t scale     = rd_u16(buf + 8);
  uint8_t  seg_count = buf[10];

  if (ver != MAP_VER) return false;
  if (seg_count < 1 || seg_count > MAP_MAX_SEGS) return false;
  (void)heading; (void)scale;  /* range is wide-open by design; renderer clamps */

  /* --- segments into a local frame: `out` stays untouched until all
   *     bytes are validated (contract: no partial output on failure). - */
  static struct MapFrame tmp;    /* ~7 KB — cannot live on the app-task stack */
  memset(&tmp, 0, sizeof tmp);

  size_t off = MAP_FRAME_HEADER_SIZE;
  for (uint8_t si = 0; si < seg_count; si++) {
    struct MapSeg *seg = &tmp.segs[si];

    if (off + 3 > len) return false;              /* type,width,npts   */
    uint8_t type  = buf[off + 0];
    uint8_t width = buf[off + 1];
    uint8_t npts  = buf[off + 2];
    off += 3;

    if (type < MAP_SEG_ROUTE || type > MAP_SEG_DESTINATION) return false;
    if (width < 1 || width > 16) return false;
    if (npts < 2 || npts > MAP_MAX_PTS) return false;

    size_t need = (size_t)npts * 4;                /* i16 dx + i16 dy  */
    if (off + need > len) return false;

    int32_t cx = 0, cy = 0;                        /* accumulated, absolute */
    for (uint8_t i = 0; i < npts; i++) {
      int16_t dx = rd_i16(buf + off + 0);
      int16_t dy = rd_i16(buf + off + 2);
      off += 4;
      if (i == 0) { cx = dx; cy = dy; }            /* first point: absolute */
      else        { cx += dx; cy += dy; }          /* rest: delta-encoded  */
      seg->points[i * 2 + 0] = clamp_i16(cx);
      seg->points[i * 2 + 1] = clamp_i16(cy);
    }

    seg->type  = type;
    seg->width = width;
    seg->npts  = npts;
  }

  /* strict: no trailing bytes (a frame is exactly header + segments) */
  if (off != len) return false;

  tmp.ver       = ver;
  tmp.seq       = seq;
  tmp.flags     = flags;
  tmp.heading   = heading;
  tmp.scale     = scale;
  tmp.seg_count = seg_count;
  memcpy(out, &tmp, sizeof *out);                  /* publish atomically */
  return true;
}
