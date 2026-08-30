/*
 * nav_message.h — Libre-Moto NavData (JSON) parser
 *
 * Header-only, no external dependencies: compiles on the host (unit tests,
 * -std=c++11) AND in the firmware (Arduino/ESP32).
 *
 * Implements protocol v1, section 2 (docs/protocol.md):
 *   { "v":1, "t":"nav|idle|reroute|arrived|beep",
 *     "maneuver":"left|right|straight|uturn|merge|slight_left|slight_right",
 *     "text":"<<=24 chars>", "dist_m":350, "limit_kmh":50, "batt_pct":87,
 *     "map_on":true, "ts":1719792000000 }
 *
 * Contract:
 *  - parse_nav_message() returns false on ANY parse error, unknown "t", or
 *    empty object — the frame must then be ignored (ble_rx_count still
 *    increments on the firmware side).
 *  - String values are escape-decoded (\\ \" \/ \b \f \n \r \t and
 *    \uXXXX incl. surrogate pairs → UTF-8). Invalid input never overflows:
 *    output is always NUL-terminated and truncated to the caller buffer.
 *  - Unknown keys are skipped (forward compatibility).
 *  - No dynamic allocation.
 */

#ifndef LIBREMOTO_NAV_MESSAGE_H
#define LIBREMOTO_NAV_MESSAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  NAV_MSG_NONE = 0,
  NAV_MSG_BEEP = 1,
  NAV_MSG_NAV,
  NAV_MSG_IDLE,
  NAV_MSG_REROUTE,
  NAV_MSG_ARRIVED
};

/* Parsed NavData message. Field meanings in docs/protocol.md §2.
 * batt_pct == -1 means "not provided". limit_kmh == 0 means "not provided". */
struct NavState {
  int      msg_type;    /* NAV_MSG_* */
  char     maneuver[16];/* maneuver keyword ("" when t != nav) */
  char     text[48];    /* short label, protocol max 24 chars + headroom */
  int      dist_m;      /* meters to maneuver, 0 = not provided */
  int      limit_kmh;   /* speed limit, 0 = not provided */
  int      batt_pct;    /* phone battery %, -1 = not provided */
  bool     map_on;      /* phone requests map view */
  uint64_t ts;          /* phone timestamp, epoch ms */
  bool     has_data;    /* true if this state came from a valid parse */
};

/* Parsed message → `out`. Returns true on success.
 * On failure `out` is left untouched. */
bool parse_nav_message(const char *json, size_t len, struct NavState *out);

/* C-only convenience (same as parse_nav_message, no name mangling risk). */
#define nav_message_parse parse_nav_message

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

#include "nav_message_parse_impl.h"

#endif

#endif /* LIBREMOTO_NAV_MESSAGE_H */
