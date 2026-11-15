/* ioexp_pcf8574.h — PCF8574 I/O expander driver, ESP-IDF. */
#ifndef IOEXP_PCF8574_H
#define IOEXP_PCF8574_H

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

bool pcf8574_init(i2c_master_bus_handle_t bus, uint8_t addr);
bool pcf8574_write_bit(int bit, int value);

/* CrowPanel LCD power + reset / touch reset sequence (call before
 * the ST7701 init stream). */
void pcf8574_panel_power_sequence(void);

#ifdef __cplusplus
}
#endif

#endif