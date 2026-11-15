/* ble_gatt.c — Libre-Moto GATT server (protocol.md §1) for ESP-IDF.
 *
 * Native port of src/ble/ble_link.h (Arduino Bluedroid): same service
 * UUIDs, same characteristic properties, same queue + poll model —
 * BLE-stack callbacks only push bytes into portMUX-guarded rings,
 * ble_link_poll() (COMM task) drains them and fires the app callbacks.
 *
 *   Service   6F1C4D52-5B66-4A3E-9C8A-3D2E7F1A4401
 *     NavData   ...4402  Write No Rsp (+Write)  phone → device (JSON)
 *     Status    ...4403  Read + Notify           device → phone (JSON)
 *     MapData   ...4404  Write No Rsp            phone → device (binary chunks)
 *     Control   ...4405  Write No Rsp (+Write)  phone → device (JSON)
 *
 * Uses the Bluedroid attribute-table API (esp_ble_gatts_create_attr_tab):
 * reads are served by the stack from the stored attribute value.
 */

#include "ble_gatt.h"

#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_common_api.h"
#include "esp_bt_defs.h"

#include "map_reassembly.h"

static const char *TAG = "ble";

/* --- UUIDs (protocol.md §1), little-endian byte arrays ------------- */
static const uint8_t SVC_UUID_LE[16]    = {0x01,0x44,0x1A,0x7F,0x2E,0x3D,0x8A,0x9C,
                                           0x3E,0x4A,0x66,0x5B,0x52,0x4D,0x1C,0x6F};
static const uint8_t CH_NAV_UUID_LE[16] = {0x02,0x44,0x1A,0x7F,0x2E,0x3D,0x8A,0x9C,
                                           0x3E,0x4A,0x66,0x5B,0x52,0x4D,0x1C,0x6F};
static const uint8_t CH_STATUS_UUID_LE[16] = {0x03,0x44,0x1A,0x7F,0x2E,0x3D,0x8A,0x9C,
                                              0x3E,0x4A,0x66,0x5B,0x52,0x4D,0x1C,0x6F};
static const uint8_t CH_MAP_UUID_LE[16] = {0x04,0x44,0x1A,0x7F,0x2E,0x3D,0x8A,0x9C,
                                           0x3E,0x4A,0x66,0x5B,0x52,0x4D,0x1C,0x6F};
static const uint8_t CH_CTRL_UUID_LE[16] = {0x05,0x44,0x1A,0x7F,0x2E,0x3D,0x8A,0x9C,
                                            0x3E,0x4A,0x66,0x5B,0x52,0x4D,0x1C,0x6F};

/* --- rings (BLE task pushes, poll pops) ---------------------------- */
#define NAV_Q_SIZE 16
#define CTRL_Q_SIZE 8

struct nav_slot { uint8_t buf[NAV_MAX_LEN]; uint16_t len; };
struct ctrl_slot { uint8_t buf[CTRL_MAX_LEN]; uint16_t len; };

static struct nav_slot s_nav_q[NAV_Q_SIZE];
static uint16_t s_nav_head = 0, s_nav_tail = 0, s_nav_free = 0;
static struct ctrl_slot s_ctrl_q[CTRL_Q_SIZE];
static uint16_t s_ctrl_head = 0, s_ctrl_tail = 0, s_ctrl_free = 0;

static struct MapReasm s_map_reasm;
static uint8_t s_map_out_buf[MAP_REASM_MAX_FRAME];

static char s_status_json[160];
static bool s_status_valid = false;

static volatile bool s_connected = false;
static volatile bool s_adv_running = false;
static volatile uint32_t s_adv_restart_at = 0;
static volatile uint32_t s_conn_params_due_ms = 0;

static struct ble_link_cb s_cb;

/* --- GATT attribute table ------------------------------------------ */
/* esp_attr_desc_t carries the UUID as a POINTER (uuid_p), so the table
 * references static UUID arrays below. */
enum {
    IDX_SVC,
    IDX_NAV_CHAR,
    IDX_NAV_VAL,
    IDX_STATUS_CHAR,
    IDX_STATUS_VAL,
    IDX_STATUS_CFG,     /* CCCD for Notify */
    IDX_MAP_CHAR,
    IDX_MAP_VAL,
    IDX_CTRL_CHAR,
    IDX_CTRL_VAL,
    HDL_COUNT
};

static uint16_t s_handle_table[HDL_COUNT];
static esp_gatt_if_t s_gatts_if = ESP_GATT_IF_NONE;
static uint16_t s_conn_id = 0;
static bool s_have_peer = false;
static esp_bd_addr_t s_peer_addr = {0};
static bool s_attr_tab_ready = false;

#define GATT_UUID_PRI_SERVICE 0x2800
#define GATT_UUID_CHAR_VAL  0x2803
#define GATT_UUID_CHAR_CFG  0x2902

static const uint16_t UUID_PRI_SERVICE = GATT_UUID_PRI_SERVICE;
static const uint16_t UUID_CHAR_VAL = GATT_UUID_CHAR_VAL;
static const uint16_t UUID_CHAR_CFG = GATT_UUID_CHAR_CFG;

/* characteristic-declaration value = properties byte */
static const uint8_t PROPS_NAV    = ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
static const uint8_t PROPS_STATUS = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static const uint8_t PROPS_MAP    = ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
static const uint8_t PROPS_CTRL   = ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
static const uint16_t CCCD_INIT = 0;

static const char STATUS_BOOT[] = "{\"v\":1,\"state\":\"boot\"}";

#define CHAR_DECL_ROW(props_ptr) \
    { .attr_control = { .auto_rsp = ESP_GATT_AUTO_RSP }, \
      .att_desc = { .uuid_length = ESP_UUID_LEN_16, \
                    .uuid_p = (uint8_t *)&UUID_CHAR_VAL, \
                    .perm = ESP_GATT_PERM_READ, \
                    .max_length = 1, \
                    .length = 1, \
                    .value = (uint8_t *)(props_ptr) } }

#define CHAR_VAL_ROW(uuid_arr, perm_, max_len, init_val, init_len) \
    { .attr_control = { .auto_rsp = ESP_GATT_AUTO_RSP }, \
      .att_desc = { .uuid_length = ESP_UUID_LEN_128, \
                    .uuid_p = (uint8_t *)(uuid_arr), \
                    .perm = (perm_), \
                    .max_length = (max_len), \
                    .length = (init_len), \
                    .value = (uint8_t *)(init_val) } }

static esp_gatts_attr_db_t s_attr_db[HDL_COUNT] = {
    [IDX_SVC] = {
        /* Bluedroid attr-table protocol (btc_gatts.c): the service row's
         * TYPE uuid is the 16-bit GATT PRI_SERVICE (0x2800); the actual
         * 128-bit service UUID travels in the VALUE field. A 128-bit type
         * uuid here makes the creation loop skip the row entirely — the
         * service is never created, all handles stay 0 and
         * esp_ble_gatts_start_service(0) logs "service not created". */
        .attr_control = { .auto_rsp = ESP_GATT_AUTO_RSP },
        .att_desc = { .uuid_length = ESP_UUID_LEN_16,
                      .uuid_p = (uint8_t *)&UUID_PRI_SERVICE,
                      .perm = ESP_GATT_PERM_READ,
                      .max_length = 16,
                      .length = 16,
                      .value = (uint8_t *)SVC_UUID_LE },
    },
    [IDX_NAV_CHAR] = CHAR_DECL_ROW(&PROPS_NAV),
    [IDX_NAV_VAL] = CHAR_VAL_ROW(CH_NAV_UUID_LE, ESP_GATT_PERM_WRITE, NAV_MAX_LEN, NULL, 0),
    [IDX_STATUS_CHAR] = CHAR_DECL_ROW(&PROPS_STATUS),
    [IDX_STATUS_VAL] = CHAR_VAL_ROW(CH_STATUS_UUID_LE, ESP_GATT_PERM_READ,
                                    sizeof s_status_json, STATUS_BOOT, sizeof STATUS_BOOT - 1),
    [IDX_STATUS_CFG] = {
        .attr_control = { .auto_rsp = ESP_GATT_AUTO_RSP },
        .att_desc = { .uuid_length = ESP_UUID_LEN_16,
                      .uuid_p = (uint8_t *)&UUID_CHAR_CFG,
                      .perm = ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                      .max_length = 2,
                      .length = 2,
                      .value = (uint8_t *)&CCCD_INIT },
    },
    [IDX_MAP_CHAR] = CHAR_DECL_ROW(&PROPS_MAP),
    [IDX_MAP_VAL] = CHAR_VAL_ROW(CH_MAP_UUID_LE, ESP_GATT_PERM_WRITE, 512, NULL, 0),
    [IDX_CTRL_CHAR] = CHAR_DECL_ROW(&PROPS_CTRL),
    [IDX_CTRL_VAL] = CHAR_VAL_ROW(CH_CTRL_UUID_LE, ESP_GATT_PERM_WRITE, CTRL_MAX_LEN, NULL, 0),
};

/* --- portMUX-guarded ring helpers ---------------------------------- */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void queue_nav(const uint8_t *data, size_t len) {
    if (len > NAV_MAX_LEN) len = NAV_MAX_LEN;
    portENTER_CRITICAL(&s_mux);
    if (s_nav_free > 0) {
        memcpy(s_nav_q[s_nav_head].buf, data, len);
        s_nav_q[s_nav_head].len = (uint16_t)len;
        s_nav_head = (uint16_t)((s_nav_head + 1) % NAV_Q_SIZE);
        s_nav_free--;
    }
    portEXIT_CRITICAL(&s_mux);
}

static void queue_ctrl(const uint8_t *data, size_t len) {
    if (len > CTRL_MAX_LEN) len = CTRL_MAX_LEN;
    portENTER_CRITICAL(&s_mux);
    if (s_ctrl_free > 0) {
        memcpy(s_ctrl_q[s_ctrl_head].buf, data, len);
        s_ctrl_q[s_ctrl_head].len = (uint16_t)len;
        s_ctrl_head = (uint16_t)((s_ctrl_head + 1) % CTRL_Q_SIZE);
        s_ctrl_free--;
    }
    portEXIT_CRITICAL(&s_mux);
}

static void map_feed(const uint8_t *data, size_t len) {
    portENTER_CRITICAL(&s_mux);
    map_reasm_feed(&s_map_reasm, data, len);
    s_map_reasm.last_chunk_ms = lm_millis();
    portEXIT_CRITICAL(&s_mux);
}

static void map_reset(void) {
    portENTER_CRITICAL(&s_mux);
    map_reasm_reset(&s_map_reasm);
    portEXIT_CRITICAL(&s_mux);
}

/* --- advertising ---------------------------------------------------- */
static void adv_start(void) {
    esp_ble_adv_params_t adv = {
        .adv_int_min = 0x20,
        .adv_int_max = 0x40,
        .adv_type = ADV_TYPE_IND,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .channel_map = ADV_CHNL_ALL,
        .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
        .peer_addr = {0},
        .peer_addr_type = BLE_ADDR_TYPE_PUBLIC,
    };
    esp_ble_gap_start_advertising(&adv);
}

/* --- GATT event handler --------------------------------------------- */
static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                esp_ble_gatts_cb_param_t *param) {
    switch (event) {
    case ESP_GATTS_REG_EVT:
        if (param->reg.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "GATTS register failed: %d", param->reg.status);
            return;
        }
        s_gatts_if = gatts_if;
        esp_ble_gatts_create_attr_tab(s_attr_db, gatts_if, HDL_COUNT, 0);
        break;

    case ESP_GATTS_CREAT_ATTR_TAB_EVT: {
        if (param->add_attr_tab.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "create attr tab failed: %d", param->add_attr_tab.status);
            return;
        }
        if (param->add_attr_tab.num_handle != HDL_COUNT) {
            ESP_LOGE(TAG, "attr tab size mismatch: %u != %d",
                     param->add_attr_tab.num_handle, HDL_COUNT);
            return;
        }
        memcpy(s_handle_table, param->add_attr_tab.handles, sizeof s_handle_table);
        esp_ble_gatts_start_service(s_handle_table[IDX_SVC]);
        s_attr_tab_ready = true;
        ESP_LOGI(TAG, "GATT service started (svc handle %u)", s_handle_table[IDX_SVC]);
        break;
    }

    case ESP_GATTS_CONNECT_EVT:
        s_connected = true;
        s_conn_id = param->connect.conn_id;
        memcpy(s_peer_addr, param->connect.remote_bda, sizeof(s_peer_addr));
        s_have_peer = true;
        s_adv_running = false;
        s_adv_restart_at = 0;
        /* Data-length extension: 251-byte packets on air (NimBLE does this
         * implicitly in the reference build). Without DLE every large
         * write is fragmented into 27-byte chunks — one map write then
         * blocks the ATT pipeline for hundreds of ms at the Android-chosen
         * connection interval and the app's paced writes get refused. */
        esp_ble_gap_set_pkt_data_len(s_peer_addr, 251);
        /* Deferred conn-param request (spec CSS A 5.2: never within the
           first 5 s — the Arduino build wedged a tablet by violating it). */
        s_conn_params_due_ms = lm_millis() + 5000;
        ESP_LOGI(TAG, "connected");
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        s_connected = false;
        s_adv_restart_at = lm_millis() + AD_RESTART_JITTER_MS;
        s_adv_running = false;
        s_conn_params_due_ms = 0;
        map_reset();
        ESP_LOGI(TAG, "disconnected (adv restart in ~3 s)");
        break;

    case ESP_GATTS_WRITE_EVT: {
        uint16_t h = param->write.handle;
        if (h == s_handle_table[IDX_NAV_VAL]) {
            queue_nav(param->write.value, param->write.len);
        } else if (h == s_handle_table[IDX_MAP_VAL]) {
            map_feed(param->write.value, param->write.len);
        } else if (h == s_handle_table[IDX_CTRL_VAL]) {
            queue_ctrl(param->write.value, param->write.len);
        }
        /* CCCD writes are auto-responded by the stack. */
        break;
    }

    case ESP_GATTS_READ_EVT:
        /* Status reads are served from the stored attribute value
           (esp_ble_gatts_set_attr_value) — nothing to do here. */
        break;

    case ESP_GATTS_EXEC_WRITE_EVT:
    case ESP_GATTS_CONF_EVT:
    default:
        break;
    }
}

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        /* esp_ble_gap_config_adv_data_raw() is an ASYNC HCI command — the
         * controller is still processing the adv-data write when it
         * returns. Starting advertising immediately collides with the
         * write and fails ("advertising start failed"). The official IDF
         * pattern (ble_throughput server): start advertising only after
         * the set-complete event. */
        if (!s_connected && !s_adv_running) {
            adv_start();
        }
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        /* btc_hci_to_esp_status() maps HCI_SUCCESS to ESP_BT_STATUS_SUCCESS
         * (0) — NOT ESP_BT_STATUS_DONE. Comparing against DONE made this
         * report 'start failed' while advertising was actually running
         * (the phone connected anyway), and worse: s_adv_running never
         * became true, so the post-disconnect restart logic stalled. */
        s_adv_running = (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS);
        ESP_LOGI(TAG, "advertising %s (status %d)",
                 s_adv_running ? "started" : "start failed",
                 param->adv_start_cmpl.status);
        break;
    default:
        break;
    }
}

/* --- public API ------------------------------------------------------ */

void ble_link_init(const struct ble_link_cb *cb) {
    s_cb = *cb;
    s_nav_free = NAV_Q_SIZE;
    s_ctrl_free = CTRL_Q_SIZE;
    s_status_json[0] = 0;

    /* Bluedroid controller + host. The ESP32-S3 has NO Bluetooth Classic —
     * the controller only supports ESP_BT_MODE_BLE (mode 1). Enabling with
     * ESP_BT_MODE_BTDM (3) is rejected: "invalid mode 3, controller support
     * mode is 1". */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_event_handler));
    ESP_ERROR_CHECK(esp_ble_gatts_app_register(0x55));

    /* Local MTU 256 — the Arduino reference build runs NimBLE with
     * CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=256 and the app's TX pacing
     * constants are tuned for that link (MapChunk=253, TX_PACE_MS=25).
     * With the Bluedroid default (517) the phone negotiates MTU 517,
     * sends 514-byte map chunks, and one Write occupies the ATT
     * pipeline for hundreds of ms (no data-length extension) — the
     * app's next paced writes are refused ('prior command is not
     * finished'), its wedge detector fires after 5 rejections and
     * tears the link down (reason 0x13). 256 matches the reference. */
    ESP_ERROR_CHECK(esp_ble_gatt_set_local_mtu(256));

    /* raw advertising payload: flags + full name */
    static uint8_t adv_data[31];
    uint8_t name_len = (uint8_t)strlen(BLE_NAME);
    adv_data[0] = 2;
    adv_data[1] = ESP_BLE_AD_TYPE_FLAG;
    adv_data[2] = ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT;
    adv_data[3] = (uint8_t)(name_len + 1);
    adv_data[4] = ESP_BLE_AD_TYPE_NAME_CMPL;
    memcpy(&adv_data[5], BLE_NAME, name_len);
    esp_ble_gap_config_adv_data_raw(adv_data, (uint8_t)(5 + name_len));
    /* advertising starts from the ADV_DATA_RAW_SET_COMPLETE event
     * (see gap_event_handler) — NOT here. */
    ESP_LOGI(TAG, "GATT server up: NavData/Status/MapData/Control");
}

void ble_link_poll(void) {
    if (!s_attr_tab_ready) return;

    /* 0. deferred conn-param update (>= 5 s after connect, spec CSS A 5.2).
     * The Arduino build used requestConnParams(15,25,0,200); the Bluedroid
     * equivalent is esp_ble_gap_update_conn_params with the peer address
     * captured in the CONNECT event. Without it Android keeps its initial
     * connection interval (30-50+ ms) — every ATT round trip outlasts the
     * app's 25 ms TX pacing window, writeCharacteristic gets refused
     * ('prior command is not finished'), and after 5 rejections the app
     * tears the link down itself (reason 0x13). */
    if (s_conn_params_due_ms && s_connected &&
        (int32_t)(lm_millis() - s_conn_params_due_ms) >= 0) {
        s_conn_params_due_ms = 0;
        if (s_have_peer) {
            esp_ble_conn_update_params_t params = {0};
            memcpy(params.bda, s_peer_addr, sizeof(params.bda));
            params.min_int = 15;          /* x1.25 ms = 18.75 ms */
            params.max_int = 25;          /* x1.25 ms = 31.25 ms */
            params.latency = 0;
            params.timeout = 200;         /* x10 ms = 2 s supervision */
            esp_err_t err = esp_ble_gap_update_conn_params(&params);
            ESP_LOGI(TAG, "conn params update requested (%s)",
                     err == ESP_OK ? "15-25 int, 2 s supervision"
                                   : esp_err_to_name(err));
        }
    }

    /* 1. drain NavData */
    static uint8_t stage[NAV_MAX_LEN];
    for (;;) {
        uint16_t len = 0;
        bool have = false;
        portENTER_CRITICAL(&s_mux);
        if (s_nav_tail != s_nav_head) {
            memcpy(stage, s_nav_q[s_nav_tail].buf, s_nav_q[s_nav_tail].len);
            len = s_nav_q[s_nav_tail].len;
            s_nav_tail = (uint16_t)((s_nav_tail + 1) % NAV_Q_SIZE);
            s_nav_free++;
            have = true;
        }
        portEXIT_CRITICAL(&s_mux);
        if (!have) break;
        if (s_cb.on_nav) s_cb.on_nav(stage, len);
    }

    /* 2. drain Control */
    static uint8_t cstage[CTRL_MAX_LEN];
    for (;;) {
        uint16_t len = 0;
        bool have = false;
        portENTER_CRITICAL(&s_mux);
        if (s_ctrl_tail != s_ctrl_head) {
            memcpy(cstage, s_ctrl_q[s_ctrl_tail].buf, s_ctrl_q[s_ctrl_tail].len);
            len = s_ctrl_q[s_ctrl_tail].len;
            s_ctrl_tail = (uint16_t)((s_ctrl_tail + 1) % CTRL_Q_SIZE);
            s_ctrl_free++;
            have = true;
        }
        portEXIT_CRITICAL(&s_mux);
        if (!have) break;
        if (s_cb.on_ctrl) s_cb.on_ctrl(cstage, len);
    }

    /* 3. MapData reassembly (deterministic magic-byte boundary) */
    {
        portENTER_CRITICAL(&s_mux);
        size_t out_len = map_reasm_poll(&s_map_reasm, s_map_out_buf, lm_millis(),
                                        MAP_CHUNK_TIMEOUT_MS);
        portEXIT_CRITICAL(&s_mux);
        if (out_len > 0 && s_cb.on_map_frame) s_cb.on_map_frame(s_map_out_buf, out_len);
    }

    /* 4. advertising restart after disconnect (3 s jitter) */
    if (!s_connected && !s_adv_running) {
        if (s_adv_restart_at != 0 && (int32_t)(lm_millis() - s_adv_restart_at) >= 0) {
            s_adv_restart_at = 0;
            adv_start();
        }
    }
}

void ble_link_update_status(const char *json) {
    size_t len = strlen(json);
    if (len >= sizeof s_status_json) len = sizeof s_status_json - 1;
    memcpy(s_status_json, json, len);
    s_status_json[len] = 0;
    s_status_valid = true;

    if (s_connected && s_attr_tab_ready) {
        /* serve reads from the stored attribute value + notify subscribers */
        esp_ble_gatts_set_attr_value(s_handle_table[IDX_STATUS_VAL],
                                     (uint16_t)len, (uint8_t *)s_status_json);
#if ESP_IDF_VERSION_MAJOR >= 6
        /* v6 signature: (gatts_if, conn_id, attr_handle, value_len, value, need_confirm) */
        esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id,
                                    s_handle_table[IDX_STATUS_VAL],
                                    (uint16_t)len, (uint8_t *)s_status_json, false);
#else
        esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id,
                                    s_handle_table[IDX_STATUS_VAL],
                                    (uint8_t *)s_status_json, (uint16_t)len, false);
#endif
    }
}

bool ble_link_connected(void) { return s_connected; }
uint32_t ble_link_rx_count(void) { return 0; }
