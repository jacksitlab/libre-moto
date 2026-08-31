#pragma once
/*---------------------------------------------------------------
 * ble_link — Libre-Moto GATT server (protocol.md §1, §3–4)
 *
 *   ESP32 = GATT server, phone = GATT client.
 *
 *   Service   6F1C4D52-5B66-4A3E-9C8A-3D2E7F1A4401
 *     NavData   ...1A4402   Write No Rsp   phone → device (JSON)
 *     Status    ...1A4403   Read + Notify  device → phone (JSON)
 *     MapData   ...1A4404   Write No Rsp   phone → device (binary,
                                          512-B chunks, 500 ms timeout)
 *     Control   ...1A4405   Write No Rsp   phone → device (JSON)
 *
 * Threading model: characteristic callbacks run in the BLE controller
 * task. They only push bytes into a mutex-guarded queue — no LVGL,
 * no parser. The main loop calls ble_link_poll() and drains the queue
 * on the ARDUINO core (same context as the serial path → UI-safe).
 *
 * API (all snake_case, project convention):
 *   ble_link_init(cb)                  — start server + advertising
 *   ble_link_poll()                    — drain queues, reassemble MapData,
 *                                        restart advertising w/ 3 s jitter
 *   ble_link_update_status(json)       — store Status payload (Read)
 *                                        + Notify subscribers
 *   ble_link_connected()               — link state
 *   ble_link_rx_count()                — NavData frames since boot
 *--------------------------------------------------------------*/

#include <Arduino.h>
#include "BLEDevice.h"   /* ESP32 BLE server + transitively FreeRTOS primitives */

#include "../libre-moto/config.h"      /* BLE_NAME */

/* --- UUIDs (protocol.md §1, provisional fixed set) ---------- */
#define SVC_UUID     "6F1C4D52-5B66-4A3E-9C8A-3D2E7F1A4401"
#define CH_NAV_UUID  "6F1C4D52-5B66-4A3E-9C8A-3D2E7F1A4402"
#define CH_STATUS_UUID "6F1C4D52-5B66-4A3E-9C8A-3D2E7F1A4403"
#define CH_MAP_UUID  "6F1C4D52-5B66-4A3E-9C8A-3D2E7F1A4404"
#define CH_CTRL_UUID "6F1C4D52-5B66-4A3E-9C8A-3D2E7F1A4405"

/* Frame sizes are bounded by protocol.md (NavData JSON ≤ few hundred
 * bytes; MapData ≤ 4 KB; Control tiny). */
#define NAV_MAX_LEN   720
#define CTRL_MAX_LEN  256
#define MAP_FRAME_MAX 4096
#define MAP_CHUNK_TIMEOUT_MS 500
#define AD_RESTART_JITTER_MS 3000

struct ble_link_cb {
  /* One complete NavData JSON frame (already queued, not thread context). */
  void (*on_nav)(const void *data, size_t len);
  /* One reassembled MapData frame (or a timed-out partial one). */
  void (*on_map_frame)(const void *data, size_t len);
  /* One Control JSON write. */
  void (*on_ctrl)(const void *data, size_t len);
};

namespace {

/* --- NavData ring (BLE task pushes, main-loop poll pops) ------ */
struct nav_slot { uint8_t buf[NAV_MAX_LEN]; uint16_t len; };
nav_slot      g_nav_q[16];
uint16_t      g_nav_head = 0;   /* next push idx        */
uint16_t      g_nav_tail = 0;   /* next pop idx         */
uint16_t      g_nav_free;       /* slot count           */
uint32_t      g_rx_count = 0;   /* NavData frames total */

/* --- Control ring --------------------------------------------- */
struct ctrl_slot { uint8_t buf[CTRL_MAX_LEN]; uint16_t len; };
ctrl_slot     g_ctrl_q[8];
uint16_t      g_ctrl_head = 0;
uint16_t      g_ctrl_tail = 0;
uint16_t      g_ctrl_free;

/* --- MapData reassembly (protocol.md §4) ---------------------- */
uint8_t       g_map_buf[MAP_FRAME_MAX];
size_t        g_map_len = 0;
uint32_t      g_map_last_chunk_ms = 0;

/* --- Status payload (for Read + Notify) ----------------------- */
char          g_status_json[160];
bool          g_status_valid = false;

/* --- Advertising state machine (driven from poll) ------------- */
bool          g_adv_running = false;
uint32_t      g_adv_restart_at = 0;   /* 0 = no restart pending */

BLEServer     *g_server = nullptr;
BLEService    *g_svc = nullptr;
BLECharacteristic *g_nav_ch = nullptr;
BLECharacteristic *g_status_ch = nullptr;
BLECharacteristic *g_map_ch = nullptr;
BLECharacteristic *g_ctrl_ch = nullptr;

ble_link_cb   g_cb{};
bool          g_connected = false;
portMUX_TYPE  g_q_mux = portMUX_INITIALIZER_UNLOCKED;

/* queue push/pop helpers (caller must not hold mux; helpers lock) */
#define NAV_PUSH_LOCK()   portENTER_CRITICAL(&g_q_mux)
#define NAV_UNLOCK()      portEXIT_CRITICAL(&g_q_mux)

void queue_nav(const uint8_t *data, size_t len) {
  if (len > NAV_MAX_LEN) len = NAV_MAX_LEN;
  NAV_PUSH_LOCK();
  if (g_nav_free > 0) {
    g_nav_q[g_nav_head].len = (uint16_t)len;
    memcpy(g_nav_q[g_nav_head].buf, data, len);
    g_nav_head = (uint16_t)((g_nav_head + 1) % 16);
    g_nav_free--;
  }
  NAV_UNLOCK();
  g_rx_count++;
}

void queue_ctrl(const uint8_t *data, size_t len) {
  if (len > CTRL_MAX_LEN) len = CTRL_MAX_LEN;
  NAV_PUSH_LOCK();
  if (g_ctrl_free > 0) {
    g_ctrl_q[g_ctrl_head].len = (uint16_t)len;
    memcpy(g_ctrl_q[g_ctrl_head].buf, data, len);
    g_ctrl_head = (uint16_t)((g_ctrl_head + 1) % 8);
    g_ctrl_free--;
  }
  NAV_UNLOCK();
}

void map_feed(const uint8_t *data, size_t len) {
  /* Append a 512-B chunk; enforce the 4 KB frame ceiling. */
  NAV_PUSH_LOCK();
  if (g_map_len + len <= MAP_FRAME_MAX) {
    memcpy(g_map_buf + g_map_len, data, len);
    g_map_len += len;
  } else {
    /* Oversized → keep nothing (drop on next timeout). */
    if (g_map_len > 0) g_map_len = 0;   /* will be handled as drop in poll */
  }
  g_map_last_chunk_ms = millis();
  NAV_UNLOCK();
}

void map_reset() {
  NAV_PUSH_LOCK();
  g_map_len = 0;
  NAV_UNLOCK();
}

} // namespace

/* ================================================================
 * Public API
 * ================================================================ */

void ble_link_init(const ble_link_cb *cb) {
  g_cb = *cb;
  g_status_json[0] = 0;
  g_nav_free = 16;
  g_ctrl_free = 8;

  BLEDevice::init(BLE_NAME);          // static init (+device name), returns bool

  /* GATT server with auto-adv on disconnect handled in ble_link_poll
   * (we own the advertising lifecycle, not the server callbacks). */
  struct server_cb : BLEServerCallbacks {
    void onConnect(BLEServer * /*pServer*/) override {
      g_connected = true;
      g_adv_running = false;
      g_adv_restart_at = 0;
      Serial.println("[BLE] connected");
    }
    void onDisconnect(BLEServer * /*pServer*/) override {
      g_connected = false;
      /* spec §6: restart advertising immediately, 3 s jitter */
      g_adv_restart_at = millis() + AD_RESTART_JITTER_MS;
      g_adv_running = false;
      map_reset();
      Serial.println("[BLE] disconnected (adv restart in ~3 s)");
    }
  } *server_callbacks = new server_cb();   /* heap-owned, lives for the app */
  g_server = BLEDevice::createServer();   /* static singleton API */
  g_server->setCallbacks(server_callbacks);

  g_svc = g_server->createService(SVC_UUID);

  /* Characteristic callbacks via small helper classes (heap-owned,
   * live for the app — the BLE stack holds the pointers internally). */
  struct nav_write_cb : BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *c) override {
      String v = c->getValue();
      queue_nav(reinterpret_cast<const uint8_t *>(v.c_str()), v.length());
    }
  };
  struct map_write_cb : BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *c) override {
      String v = c->getValue();
      map_feed(reinterpret_cast<const uint8_t *>(v.c_str()), v.length());
    }
  };
  struct ctrl_write_cb : BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *c) override {
      String v = c->getValue();
      queue_ctrl(reinterpret_cast<const uint8_t *>(v.c_str()), v.length());
    }
  };
  struct status_read_cb : BLECharacteristicCallbacks {
    void onRead(BLECharacteristic *c) override {
      if (!g_status_valid) {
        const char *boot = "{\"v\":1,\"state\":\"boot\"}";
        c->setValue(reinterpret_cast<const uint8_t *>(boot), strlen(boot));
      } else {
        c->setValue(reinterpret_cast<const uint8_t *>(g_status_json),
                    strlen(g_status_json));
      }
    }
  };

  /* NavData — Write No Rsp (spec) + Write Rsp (nRF-Connect test path) */
  g_nav_ch = g_svc->createCharacteristic(CH_NAV_UUID,
      BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_WRITE);
  g_nav_ch->setCallbacks(new nav_write_cb());

  /* Status — Read + Notify (device → phone) */
  g_status_ch = g_svc->createCharacteristic(CH_STATUS_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  g_status_ch->setCallbacks(new status_read_cb());

  /* MapData — Write No Rsp (binary chunks) */
  g_map_ch = g_svc->createCharacteristic(CH_MAP_UUID,
      BLECharacteristic::PROPERTY_WRITE_NR);
  g_map_ch->setCallbacks(new map_write_cb());

  /* Control — Write No Rsp (spec) + Write Rsp (nRF-Connect test path) */
  g_ctrl_ch = g_svc->createCharacteristic(CH_CTRL_UUID,
      BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_WRITE);
  g_ctrl_ch->setCallbacks(new ctrl_write_cb());

  g_svc->start();

  /* Initial advertising (no restart pending yet) */
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->start();
  g_adv_running = true;
}

void ble_link_poll() {
  if (!g_server) return;

  /* 1. Drain NavData queue → callback on ARDUINO core.
   * Collect pending slots first (mutex), then fire outside the lock. */
  static uint8_t stage[NAV_MAX_LEN];
  for (;;) {
    uint8_t *slot = nullptr;
    uint16_t len = 0;
    NAV_PUSH_LOCK();
    if (g_nav_tail != g_nav_head) {
      slot = g_nav_q[g_nav_tail].buf;
      len = g_nav_q[g_nav_tail].len;
      g_nav_tail = (uint16_t)((g_nav_tail + 1) % 16);
      g_nav_free++;
    } else {
      g_nav_tail = g_nav_head; /* sanity */
    }
    NAV_UNLOCK();
    if (slot == nullptr) break;
    memcpy(stage, slot, len);
    if (g_cb.on_nav) g_cb.on_nav(stage, len);
  }

  /* 2. Drain Control queue. */
  {
    static uint8_t cstage[CTRL_MAX_LEN];
    for (;;) {
      uint8_t *slot = nullptr;
      uint16_t len = 0;
      NAV_PUSH_LOCK();
      if (g_ctrl_tail != g_ctrl_head) {
        slot = g_ctrl_q[g_ctrl_tail].buf;
        len = g_ctrl_q[g_ctrl_tail].len;
        g_ctrl_tail = (uint16_t)((g_ctrl_tail + 1) % 8);
        g_ctrl_free++;
      }
      NAV_UNLOCK();
      if (slot == nullptr) break;
      memcpy(cstage, slot, len);
      if (g_cb.on_ctrl) g_cb.on_ctrl(cstage, len);
    }
  }

  /* 3. MapData reassembly + 500 ms timeout (spec §4/§6). */
  {
    bool has = false;
    NAV_PUSH_LOCK();
    has = (g_map_len > 0);
    uint32_t last = g_map_last_chunk_ms;
    NAV_UNLOCK();
    if (has && (millis() - last) > MAP_CHUNK_TIMEOUT_MS) {
      /* Frame closed (complete or timed-out partial). */
      size_t len = g_map_len;
      uint8_t *frame = nullptr;
      NAV_PUSH_LOCK();
      frame = g_map_buf;
      g_map_len = 0;
      NAV_UNLOCK();
      if (g_cb.on_map_frame) g_cb.on_map_frame(frame, len);
    }
  }

  /* 4. Advertising state machine (disconnect → 3 s jitter → restart). */
  if (!g_connected && !g_adv_running) {
    if (g_adv_restart_at != 0 && millis() >= g_adv_restart_at) {
      g_adv_restart_at = 0;
      BLEAdvertising *adv = BLEDevice::getAdvertising();
      adv->stop();
      adv->start();
      g_adv_running = true;
    }
  }
}

void ble_link_update_status(const char *json) {
  if (!g_status_ch) return;
  size_t len = strlen(json);
  if (len >= sizeof(g_status_json))
    len = sizeof(g_status_json) - 1;
  memcpy(g_status_json, json, len);
  g_status_json[len] = 0;
  g_status_valid = true;

  if (g_connected) {
    /* Read serves the stored payload; Notify pushes it to subscribers. */
    g_status_ch->setValue(reinterpret_cast<const uint8_t *>(g_status_json),
                          strlen(g_status_json));
    g_status_ch->notify();
  }
}

bool ble_link_connected() { return g_connected; }
uint32_t ble_link_rx_count() { return g_rx_count; }
bool ble_link_map_reset() { map_reset(); return true; }
