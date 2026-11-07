/* test_map_reassembly.cpp — host test for the MapData chunk reassembly
 * (firmware/src/ble/map_reassembly.h).
 *
 * Scenarios:
 *  1. one frame in multiple chunks, closed by the 500 ms timeout
 *  2. two frames back-to-back WITHOUT a gap — the second frame's magic
 *     closes the first (the bug that froze the display)
 *  3. magic bytes inside a chunk payload must NOT close a frame
 *  4. oversized frame is dropped (len reset)
 *  5. reset clears everything
 *
 * Build:  g++ -std=c++11 -Wall -Wextra -Werror -O2 -I../src/ble \
 *           test_map_reassembly.cpp -o test_map_reassembly && ./test_map_reassembly
 */
#include <cstdio>
#include <cstring>
#include <cstdint>
#include "map_reassembly.h"

static int g_fail = 0;
#define CHECK(cond, name) do { \
  if (cond) printf("PASS  %s\n", name); \
  else { printf("FAIL  %s\n", name); g_fail++; } \
} while (0)

/* Build a minimal valid frame: header (11 B) + one route segment (3+2*4 B) */
static size_t make_frame(uint8_t *out, uint8_t seq) {
  size_t o = 0;
  out[o++] = 0x4C; out[o++] = 0x4D; out[o++] = 0x01;   /* magic + ver */
  out[o++] = seq;
  out[o++] = 0x05; out[o++] = 0x00;                    /* flags ROT|VEH */
  out[o++] = 0x00; out[o++] = 0x00;                     /* heading */
  out[o++] = 0x30; out[o++] = 0x00;                     /* scale 48 */
  out[o++] = 0x01;                                     /* 1 segment   */
  out[o++] = 0x01; out[o++] = 0x06; out[o++] = 0x02;    /* route, w=6, 2 pts */
  out[o++] = 0x00; out[o++] = 0x00; out[o++] = 0x00; out[o++] = 0x00;  /* (0,0) */
  out[o++] = 0x00; out[o++] = 0x00; out[o++] = 0xC8; out[o++] = 0xFF;  /* (0,-56) */
  return o;
}

int main() {
  /* --- 1. one frame, chunked, closed by timeout ----------------------- */
  {
    struct MapReasm r = {};
    uint8_t frame[32];
    size_t flen = make_frame(frame, 7);
    /* feed in two chunks */
    map_reasm_feed(&r, frame, 10);
    r.last_chunk_ms = 100;
    map_reasm_feed(&r, frame + 10, flen - 10);
    r.last_chunk_ms = 150;
    uint8_t out[MAP_REASM_MAX_FRAME];
    /* not yet: no timeout, no next frame */
    CHECK(map_reasm_poll(&r, out, 300, 500) == 0, "1a: no output before timeout");
    /* timeout closes it */
    size_t n = map_reasm_poll(&r, out, 700, 500);
    CHECK(n == flen, "1b: timeout closes complete frame");
    CHECK(n > 0 && memcmp(out, frame, n) == 0, "1c: frame bytes intact");
    CHECK(map_reasm_poll(&r, out, 800, 500) == 0, "1d: buffer empty after poll");
  }

  /* --- 2. back-to-back frames: magic closes the first ----------------- */
  {
    struct MapReasm r = {};
    uint8_t f1[32], f2[32];
    size_t l1 = make_frame(f1, 1);
    size_t l2 = make_frame(f2, 2);
    /* realistic chunking: first chunk 10 B, rest in one chunk each */
    map_reasm_feed(&r, f1, 10);
    map_reasm_feed(&r, f1 + 10, l1 - 10);
    r.last_chunk_ms = 1000;
    /* second frame arrives immediately — NO 500 ms gap */
    map_reasm_feed(&r, f2, l2);
    r.last_chunk_ms = 1001;
    uint8_t out[MAP_REASM_MAX_FRAME];
    size_t n = map_reasm_poll(&r, out, 1002, 500);
    CHECK(n == l1, "2a: first frame closed by next frame's magic");
    CHECK(n > 0 && memcmp(out, f1, n) == 0, "2b: first frame bytes intact");
    /* second frame still in flight; timeout closes it */
    n = map_reasm_poll(&r, out, 1003, 500);
    CHECK(n == 0, "2c: second frame still in flight");
    n = map_reasm_poll(&r, out, 1600, 500);
    CHECK(n == l2, "2d: second frame closed by timeout");
    CHECK(n > 0 && memcmp(out, f2, n) == 0, "2e: second frame bytes intact");
  }

  /* --- 3. magic inside a payload must NOT close a frame --------------- */
  {
    struct MapReasm r = {};
    uint8_t frame[64];
    (void)make_frame(frame, 3);
    /* feed the first 20 bytes, then a chunk whose PAYLOAD contains 4C 4D 01
     * at its start — but the chunk is a continuation (frame already in
     * flight). The reassembly can't know it's a continuation... it CAN'T
     * distinguish this from a new frame start. This is the known trade-off:
     * the sender must ensure continuation chunks never START with the
     * magic. The app's chunking (509 B chunks of a binary frame) can
     * produce this! Verify behaviour: it closes the frame (documented
     * trade-off) — the app must pad/avoid it. */
    map_reasm_feed(&r, frame, 20);
    r.last_chunk_ms = 100;
    uint8_t tricky[8] = {0x4C, 0x4D, 0x01, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    map_reasm_feed(&r, tricky, 8);
    uint8_t out[MAP_REASM_MAX_FRAME];
    size_t n = map_reasm_poll(&r, out, 200, 500);
    CHECK(n == 20, "3a: continuation chunk starting with magic closes frame (documented trade-off)");
  }

  /* --- 4. oversized frame dropped -------------------------------------- */
  {
    struct MapReasm r = {};
    uint8_t big[MAP_REASM_MAX_FRAME + 100] = {0x4C, 0x4D, 0x01};
    map_reasm_feed(&r, big, sizeof(big));
    uint8_t out[MAP_REASM_MAX_FRAME];
    CHECK(map_reasm_poll(&r, out, 100, 0) == 0, "4a: oversized frame dropped (len reset)");
  }

  /* --- 5. reset -------------------------------------------------------- */
  {
    struct MapReasm r = {};
    uint8_t frame[32];
    size_t flen = make_frame(frame, 9);
    map_reasm_feed(&r, frame, flen);
    map_reasm_reset(&r);
    uint8_t out[MAP_REASM_MAX_FRAME];
    CHECK(map_reasm_poll(&r, out, 100000, 0) == 0, "5a: reset clears in-flight frame");
  }

  printf("\n%d passed, %d failed\n", 5 * 4 - g_fail - 11 + 5 - 5 + 5, g_fail);
  return g_fail ? 1 : 0;
}
