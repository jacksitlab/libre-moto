/* backlight.h */
#ifndef BACKLIGHT_H
#define BACKLIGHT_H

#ifdef __cplusplus
extern "C" {
#endif

void backlight_init(void);
void backlight_set(int pct);
int backlight_get(void);

#ifdef __cplusplus
}
#endif

#endif
