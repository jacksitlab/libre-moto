/*
 * map_reassembly.h — pure MapData chunk-reassembly state machine
 *
 * Extracted from ble_link.h so it is host-testable (like map_frame.h):
 * no Arduino, no BLE, no globals beyond one struct. The firmware's
 * map_feed() + poll stage delegate to this.
 *
 * Frame boundary is DETERMINISTIC: a chunk starting with the frame
 * magic (4C 4D 01 = "LM" + ver 1, little-endian) closes the in-flight
 * frame. The 500 ms gap remains only as a fallback for the last frame
 * of a burst (caller checks map_reassembly_timeout()).
 *
 * Concurrency: the caller (ble_link) guards the struct with its mux.
 */
#ifndef LIBREMOTO_MAP_REASSEMBLY_H
#define LIBREMOTO_MAP_REASSEMBLY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define MAP_REASM_MAX_FRAME 4096

struct MapReasm {
  uint8_t  buf[MAP_REASM_MAX_FRAME];   /* in-flight frame   */
  size_t   len;
  uint8_t  done[MAP_REASM_MAX_FRAME];  /* closed, awaiting poll */
  size_t   done_len;
  uint32_t last_chunk_ms;
};

static inline void map_reasm_reset(struct MapReasm *r) {
  r->len = 0;
  r->done_len = 0;
}

/* Feed one BLE chunk. Closes the in-flight frame when this chunk starts
 * a new one: the chunk begins with the frame magic (4C 4D 01).
 *
 * SENDER CONTRACT (BleController.sendMap enforces this): a continuation
 * chunk must never start with 4C 4D 01. The app scans the frame bytes at
 * send time and shifts any offending chunk boundary by one byte, so the
 * firmware never sees a false frame start. */
static inline void map_reasm_feed(struct MapReasm *r, const uint8_t *data, size_t len) {
  bool is_frame_start = (len >= 3 && data[0] == 0x4C && data[1] == 0x4D && data[2] == 0x01);
  if (is_frame_start && r->len > 0) {
    /* close the previous frame; poll hands it to the parser */
    memcpy(r->done, r->buf, r->len);
    r->done_len = r->len;
    r->len = 0;
  }
  if (r->len + len <= MAP_REASM_MAX_FRAME) {
    memcpy(r->buf + r->len, data, len);
    r->len += len;
  } else {
    r->len = 0;   /* oversized → drop (handled as parse error in poll) */
  }
}

/* True when the in-flight frame saw no chunk for `timeout_ms'. */
static inline bool map_reasm_timed_out(const struct MapReasm *r, uint32_t now_ms, uint32_t timeout_ms) {
  return r->len > 0 && (now_ms - r->last_chunk_ms) > timeout_ms;
}

/* Poll: returns the length of the next COMPLETE frame (closed by magic
 * or by timeout) and copies it into `out', or 0 when none is ready. */
static inline size_t map_reasm_poll(struct MapReasm *r, uint8_t *out, uint32_t now_ms, uint32_t timeout_ms) {
  if (r->done_len > 0) {
    memcpy(out, r->done, r->done_len);
    size_t n = r->done_len;
    r->done_len = 0;
    return n;
  }
  if (map_reasm_timed_out(r, now_ms, timeout_ms)) {
    memcpy(out, r->buf, r->len);
    size_t n = r->len;
    r->len = 0;
    return n;
  }
  return 0;
}

#endif /* LIBREMOTO_MAP_REASSEMBLY_H */
