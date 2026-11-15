/* splash.h */
#ifndef SPLASH_H
#define SPLASH_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Static splash: fill black + blit the I8 logo into the panel fb. */
void splash_paint(uint16_t *panel_fb);

/* Animated splash (website variant A "assembly"): start the timeline,
 * then call splash_anim_tick() at ~50 fps from the boot loop until
 * splash_anim_done() reports the final frame (identical to the static
 * logo). */
void splash_anim_start(void);
void splash_anim_tick(uint16_t *panel_fb);
bool splash_anim_done(void);

#ifdef __cplusplus
}
#endif

#endif
