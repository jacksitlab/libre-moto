/* touch_cst816.h — CST816 touch driver (I2C), ESP-IDF. */
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