/*
 * map_frame.h — Libre-Moto MapData (binary) frame parser
 *
 * Header-only, no external dependencies: compiles on the host (unit tests,
 * -std=c++11) AND in the firmware (Arduino/ESP32).
 *
 * Implements protocol v1, section 4 (docs/protocol.md), little-endian:
 *
 *   Offset  Size   Field        Type
 *   0       2      magic        u16   0x4D4C ("LM")
 *   2       1      ver          u8    (1)
 *   3       1      seq          u8    0..255
 *   4       2      flags        u16   bit0 rotate w/ heading, bit1 scale bar,
 *                                     bit2 vehicle arrow (default 1)
 *   6       2      heading      i16   degrees × 10
 *   8       2      scale        u16   px_per_meter × 100
 *   10      1      seg_count    u8    1..30
 *   11      ...    segments     seg_count × Segment
 *
 *   SEGMENT (3 header bytes + 4·npts payload):
 *     0      type    u8   1=route, 2=road, 3=branch, 4=destination
 *     1      width   u8   1..8 px
 *     2      npts    u8   2..120
 *     3      px[]    i16 dx,dy — delta-encoded (1st absolute, rest relative)
 *
 * Contract:
 *  - parse_map_frame() returns false on ANY error: bad magic, unknown ver,
 *    out-of-range field, truncated segment, or trailing bytes.
 *  - `out.segs[N].points[]` are the ABSOLUTE, decoded coordinates (device
 *    renders them as-is; origin = display center).
 *  - No dynamic allocation. Static arrays, bounded by the protocol maxima.
 */

#ifndef LIBREMOTO_MAP_FRAME_H
#define LIBREMOTO_MAP_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAP_MAGIC      0x4D4C   /* "LM" */
#define MAP_VER        1
#define MAP_MAX_SEGS   30
#define MAP_MAX_PTS    120

/* Flag bits (protocol.md §4) */
#define MAP_FLAG_ROTATE      (1u << 0)
#define MAP_FLAG_SCALE_BAR   (1u << 1)
#define MAP_FLAG_VEHICLE     (1u << 2)

/* Segment kinds */
enum {
  MAP_SEG_ROUTE       = 1,
  MAP_SEG_ROAD        = 2,
  MAP_SEG_BRANCH      = 3,
  MAP_SEG_DESTINATION = 4
};

/* One decoded segment: header + absolute point list (2 ints per point: x, y). */
struct MapSeg {
  uint8_t type;                 /* MAP_SEG_* */
  uint8_t width;                /* line width px, 1..8 */
  uint8_t npts;                 /* 2..120 */
  int16_t points[MAP_MAX_PTS * 2]; /* absolute screen coordinates, x0 y0 x1 y1 ... */
};

/* Decoded frame. `segs[]` are the MAP_MAX_SEGS largest, but only the first
 * `seg_count` entries are valid. */
struct MapFrame {
  uint8_t  ver;
  uint8_t  seq;
  uint16_t flags;               /* MAP_FLAG_* */
  int16_t  heading;             /* degrees × 10 */
  uint16_t scale;               /* px_per_meter × 100 */
  uint8_t  seg_count;           /* 1..MAP_MAX_SEGS */
  struct MapSeg segs[MAP_MAX_SEGS];
};

/* Decode a binary MapData frame (protocol §4) → `out`.
 * Returns true on success. On failure `out` is left untouched. */
bool parse_map_frame(const uint8_t *buf, size_t len, struct MapFrame *out);

/* Byte size of the fixed frame header (magic..seg_count, offsets 0..10). */
#define MAP_FRAME_HEADER_SIZE 11

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

#include "map_frame_parse_impl.h"

#endif

#endif /* LIBREMOTO_MAP_FRAME_H */
