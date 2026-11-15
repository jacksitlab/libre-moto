/* ble_gatt.h — Libre-Moto GATT server for ESP-IDF (protocol.md §1).
 *
 * Same external contract as src/ble/ble_link.h (the Arduino build):
 * callbacks push into queues, ble_link_poll() drains on the COMM task.
 */
#ifndef BLE_GATT_ESPIDF_H
#define BLE_GATT_ESPIDF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../libre-moto/config.h"   /* BLE_NAME (+ AppState enum, C++-safe) */

#ifdef __cplusplus
extern "C" {
#endif

/* Frame sizes (protocol.md) */
#ifndef NAV_MAX_LEN
#define NAV_MAX_LEN   720
#endif
#ifndef CTRL_MAX_LEN
#define CTRL_MAX_LEN  256
#endif
#ifndef MAP_FRAME_MAX
#define MAP_FRAME_MAX 4096
#endif
#define MAP_CHUNK_TIMEOUT_MS 500
#define AD_RESTART_JITTER_MS 3000

struct ble_link_cb {
    void (*on_nav)(const void *data, size_t len);
    void (*on_map_frame)(const void *data, size_t len);
    void (*on_ctrl)(const void *data, size_t len);
};

void ble_link_init(const struct ble_link_cb *cb);
void ble_link_poll(void);
void ble_link_update_status(const char *json);
bool ble_link_connected(void);
uint32_t ble_link_rx_count(void);

/* millis() replacement provided by app_main.c (esp_timer-based). */
uint32_t lm_millis(void);

#ifdef __cplusplus
}
#endif

#endif