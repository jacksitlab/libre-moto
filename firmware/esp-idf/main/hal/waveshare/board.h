/* board.h — per-board hardware bring-up contract.
 *
 * Waveshare ESP32-S3-Touch-AMOLED-1.75C: no PCF8574 expander, no
 * backlight pin (AMOLED is self-emissive). The CO5300 panel driver
 * handles its own reset (GPIO1). All contract functions are no-ops.
 */
#ifndef LM_BOARD_H
#define LM_BOARD_H

#include <stdbool.h>
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

bool board_power_init(i2c_master_bus_handle_t i2c_bus);
void backlight_init(void);
void backlight_set(int pct);
int  backlight_get(void);

#ifdef __cplusplus
}
#endif

#endif
