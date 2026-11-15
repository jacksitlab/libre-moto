/* LibreMoto splash animation — GENERATED from docs/logo.svg. */
#ifndef SPLASH_ANIM_H
#define SPLASH_ANIM_H
#include <stdint.h>

typedef struct {
    const uint8_t *mask;   /* A8 alpha, w*h bytes */
    uint16_t rgb565;       /* flat element color */
    uint8_t  anim;         /* 0 chIn 1 dotIn 2 streakIn 3 fromLeft 4 fromRight 5 settle */
    uint16_t delay_ms, dur_ms;
    int16_t  x, y;         /* placement in the 480x188 splash area */
    uint16_t w, h;
} splash_elem_t;

extern const splash_elem_t splash_elems[];
extern const int splash_elem_count;
extern const int splash_anim_total_ms;
#endif
