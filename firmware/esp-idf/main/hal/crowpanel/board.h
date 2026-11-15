/* board.h — per-board hardware bring-up contract.
 *
 * Called from app_main BEFORE panel_init(): everything the board needs
 * before the display can show anything (power sequencing, backlight).
 * The app code never touches board-specific devices directly.
 */
#ifndef LM_BOARD_H
#define LM_BOARD_H

#include <stdbool.h>
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Power/reset sequencing + backlight bring-up. Returns false when a
 * mandatory board device is missing (the app logs and continues —
 * serial commands still work). */
bool board_power_init(i2c_master_bus_handle_t i2c_bus);

/* Backlight brightness (0-100 %). Boards without a backlight pin
 * implement these as no-ops. */
void backlight_init(void);
void backlight_set(int pct);
int  backlight_get(void);

#ifdef __cplusplus
}
#endif

#endif
