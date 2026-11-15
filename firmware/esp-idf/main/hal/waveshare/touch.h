/* touch_cst9217.h — CST9217 touch driver (I2C), ESP-IDF.
 *
 * Waveshare ESP32-S3-Touch-AMOLED-1.75C touch frontend.
 *
 * STATUS: stub. Same contract as the CrowPanel's CST816 driver:
 * touch_init() on the shared I2C bus, touch_touched()/touch_get_point()
 * polled from app_main's gesture loop. Pins (Waveshare wiki):
 *   SDA=GPIO15  SCL=GPIO14  INT=GPIO11  RESET=GPIO2, I2C addr 0x5A.
 */
#ifndef LM_TOUCH_H
#define LM_TOUCH_H

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

bool touch_init(i2c_master_bus_handle_t bus);
bool touch_touched(void);
void touch_get_point(int16_t *x, int16_t *y);

#ifdef __cplusplus
}
#endif

#endif
