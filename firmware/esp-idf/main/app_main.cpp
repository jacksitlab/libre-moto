/* app_main.c — Libre-Moto firmware, ESP-IDF port of the Arduino sketch.
 *
 * Architecture is carried over 1:1 from firmware/src/libre-moto/libre-moto.ino
 * (the reference build — see the comments there for the full history):
 *
 *   DISPLAY task (app_main loop): drains ui_q, blits prepared map
 *     frames, touch gestures, state timeouts, lv_timer_handler().
 *     ALL LVGL calls live here.
 *   COMM task: ble_link_poll() → parse → prepare_map_frame, publishes
 *     g_map_out + ui_q under mutexes. NO LVGL calls.
 *
 * The map screen renders OUTSIDE LVGL (map_blit painters + one memcpy
 * into the panel framebuffer) — the sketch's verified DIRECT-mode
 * architecture. LVGL handles home/info screens and toasts only.
 *
 * Hardware: CrowPanel 2.1" (ESP32-S3R8, ST7701 RGB 480x480, CST816
 * touch, PCF8574 expander) — see docs/hardware.md.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/i2c_master.h"

#include "lvgl.h"

#include "config.h"
#include "board.h"
#include "panel.h"
#include "touch.h"
#include "splash.h"
#include "splash_anim.h"
#include "serial_cmd.h"
#include "ble_gatt.h"

#include "nav_message.h"
#include "map_frame.h"
#include "map_render.h"
#include "map_blit.h"

#include "libremoto_icon.h"
#include "libremoto_logo.h"
#include "maneuver_icons.h"
#include "vehicle_icon.h"

static const char *TAG = "libremoto";

/* ================================================================
 * millis() replacement (esp_timer, ms since boot)
 * ================================================================ */
uint32_t lm_millis(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ================================================================
 * LVGL image descriptors (same assets as the sketch)
 * ================================================================ */
static const lv_image_dsc_t libremoto_icon = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_ARGB8888,
        .flags = 0,
        .w = 200,
        .h = 200,
        .stride = 200 * 4,
        .reserved_2 = 0,
    },
    .data_size = 200 * 200 * 4,
    .data = libremoto_icon_map,
    .reserved = NULL,
    .reserved_2 = NULL,
};

static const lv_image_dsc_t libremoto_logo = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_I8,
        .flags = 0,
        .w = 480,
        .h = 188,
        .stride = 480,
        .reserved_2 = 0,
    },
    .data_size = 256 * 4 + 480 * 188,
    .data = libremoto_logo_map,
    .reserved = NULL,
    .reserved_2 = NULL,
};

#define MANEUVER_ICON_DSC(name) \
    static const lv_image_dsc_t maneuver_icon_##name = { \
        .header = { \
            .magic = LV_IMAGE_HEADER_MAGIC, \
            .cf = LV_COLOR_FORMAT_ARGB8888, \
            .flags = 0, \
            .w = 48, .h = 48, \
            .stride = 48 * 4, \
            .reserved_2 = 0, \
        }, \
        .data_size = 48 * 48 * 4, \
        .data = maneuver_##name##_map, \
        .reserved = NULL, \
        .reserved_2 = NULL, \
    };
MANEUVER_ICON_DSC(left)
MANEUVER_ICON_DSC(right)
MANEUVER_ICON_DSC(straight)
MANEUVER_ICON_DSC(uturn)
MANEUVER_ICON_DSC(merge)
MANEUVER_ICON_DSC(slight_left)
MANEUVER_ICON_DSC(slight_right)

static const lv_image_dsc_t vehicle_icon = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_ARGB8888,
        .flags = 0,
        .w = 64,
        .h = 64,
        .stride = 64 * 4,
        .reserved_2 = 0,
    },
    .data_size = 64 * 64 * 4,
    .data = vehicle_icon_map,
    .reserved = NULL,
    .reserved_2 = NULL,
};

static const uint8_t *maneuver_icon_map(const char *dir) {
    if      (!strcmp(dir, "left"))          return maneuver_left_map;
    else if (!strcmp(dir, "right"))       return maneuver_right_map;
    else if (!strcmp(dir, "uturn"))       return maneuver_uturn_map;
    else if (!strcmp(dir, "merge"))       return maneuver_merge_map;
    else if (!strcmp(dir, "slight_left")) return maneuver_slight_left_map;
    else if (!strcmp(dir, "slight_right")) return maneuver_slight_right_map;
    else                                   return maneuver_straight_map;
}

static const lv_image_dsc_t *maneuver_icon(const char *dir) {
    if      (!strcmp(dir, "left"))        return &maneuver_icon_left;
    else if (!strcmp(dir, "right"))       return &maneuver_icon_right;
    else if (!strcmp(dir, "uturn"))       return &maneuver_icon_uturn;
    else if (!strcmp(dir, "merge"))       return &maneuver_icon_merge;
    else if (!strcmp(dir, "slight_left")) return &maneuver_icon_slight_left;
    else if (!strcmp(dir, "slight_right")) return &maneuver_icon_slight_right;
    else                                   return &maneuver_icon_straight;
}

/* ================================================================
 * State (mirrors the sketch's statics)
 * ================================================================ */
static const uint16_t screen_width = SCREEN_W;
static const uint16_t screen_height = SCREEN_H;

static int brightness_pct = 70;
static nvs_handle_t s_nvs;

static lv_obj_t *home_screen = NULL;
static lv_obj_t *home_icon = NULL;
static lv_obj_t *ble_dot = NULL;
static lv_obj_t *toast_label = NULL;
static lv_timer_t *toast_timer = NULL;

/* MAP_CX/MAP_CY come from map_render.h (240/320 as floats — same values
   the sketch used; the C++ build forbids redefining the macro). */
static uint16_t *g_map_fb = NULL;          /* PSRAM offscreen map fb */
static uint16_t *g_panel_fb = NULL;       /* DMA-scanned panel fb */

static lv_obj_t *map_screen = NULL;
static lv_obj_t *map_vehicle = NULL;
static lv_obj_t *map_marker = NULL;
static lv_obj_t *map_scale = NULL;
static lv_obj_t *map_label = NULL;
static lv_obj_t *map_nav_panel = NULL;
static lv_obj_t *map_nav_arrow = NULL;
static lv_obj_t *map_nav_dist = NULL;
static lv_obj_t *map_limit = NULL;
static lv_obj_t *map_limit_label = NULL;

static lv_obj_t *active_screen = NULL;
static lv_timer_t *ble_timer = NULL;

static NavState g_nav;
static bool have_nav = false;
static uint32_t rx_count = 0, rx_ok_count = 0;
static uint32_t last_data_ms = 0;
static uint32_t g_splash_until_ms = 0;
static AppState app = APP_IDLE;

static uint8_t map_seq_last = 0;
static uint32_t map_drop_count = 0;
static bool map_seq_seen = false;
static uint32_t state_until_ms = 0;

/* ---- threading hand-off (comm → display) ---- */
static SemaphoreHandle_t g_map_mux = NULL;
static MapRenderFrame g_map_out;
static volatile bool g_map_ready = false;

static SemaphoreHandle_t g_ui_mux = NULL;
static SemaphoreHandle_t g_map_parse_mux = NULL;
#define UI_Q_SIZE 16
struct ui_msg {
    int kind;
    NavState nav;
    CtrlState ctrl;
};
static struct ui_msg ui_q[UI_Q_SIZE];
static uint16_t ui_head = 0, ui_tail = 0, ui_free = 0;
enum { UI_MSG_NAV = 1, UI_MSG_CTRL };

static void ui_push(const struct ui_msg *m) {
    if (g_ui_mux == NULL) return;
    xSemaphoreTake(g_ui_mux, portMAX_DELAY);
    if (ui_free > 0) {
        ui_q[ui_head] = *m;
        ui_head = (uint16_t)((ui_head + 1) % UI_Q_SIZE);
        ui_free--;
    }
    xSemaphoreGive(g_ui_mux);
}

/* colors (dark theme) */
static const lv_color_t C_BG     = lv_color_hex(0x0D1117);
static const lv_color_t C_TEXT   = lv_color_hex(0xE6EDF3);
static const lv_color_t C_TEXT2  = lv_color_hex(0x8B949E);
static const lv_color_t C_ACCENT = lv_color_hex(0x3FB950);
static const lv_color_t C_WARN   = lv_color_hex(0xD29922);
static const lv_color_t C_BAD    = lv_color_hex(0xF85149);
static lv_obj_t *info_screen = NULL;
static lv_obj_t *info_title = NULL;
static lv_obj_t *info_sub = NULL;

/* ================================================================
 * Forward decls
 * ================================================================ */
static void goto_screen(lv_obj_t *scr, const char *name);
static void toast(const char *text, lv_color_t color);
static void on_nav_state(const NavState *st);
static void render_map_display(const MapRenderFrame *rf);
static void dr_repaint(const MapRenderFrame *rf, int dy);

/* ================================================================
 * LVGL display output
 * ================================================================ */
static uint16_t s_flush_scratch[SCREEN_W];

static void my_disp_flush(lv_display_t *display, const lv_area_t *area, uint8_t *px_map) {
    const uint32_t w = (area->x2 - area->x1 + 1);
    const uint32_t h = (area->y2 - area->y1 + 1);
    /* DIRECT-mode stride semantics (verified in the sketch — see the
       long comment in libre-moto.ino): px_map IS the full-screen buffer,
       the flushed area is a WINDOW inside it. Absolute row indexing. */
    const int32_t scr_w = lv_display_get_horizontal_resolution(display);
    const uint16_t *src = (const uint16_t *)px_map;
    for (uint32_t row = 0; row < h; ++row) {
        const uint16_t *srow = src + (size_t)(area->y1 + (int32_t)row) * (uint32_t)scr_w + area->x1;
        uint16_t *drow = s_flush_scratch;
        const uint16_t *dend = drow + w;
        while (drow < dend) {
            const uint16_t c = *srow++;
            *drow++ = (uint16_t)((c & 0x07E0) | ((c & 0x001F) << 11) | ((c & 0xF800) >> 11));
        }
        /* panel fb is DMA-scanned: paint straight into it (R/B swap above) */
        uint16_t *dst = g_panel_fb + (size_t)(area->y1 + (int32_t)row) * (uint32_t)scr_w + area->x1;
        memcpy(dst, s_flush_scratch, w * sizeof(uint16_t));
    }
    lv_display_flush_ready(display);
}

/* ================================================================
 * Touch input (LVGL indev)
 * ================================================================ */
static void my_touchpad_read(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    if (touch_touched()) {
        int16_t px, py;
        touch_get_point(&px, &py);
        static int16_t stable_x = -1;
        static int16_t stable_y = -1;
        int16_t touch_x = px < 0 ? 0 : (px > screen_width - 1 ? screen_width - 1 : px);
        int16_t touch_y = py - TOUCH_Y_OFFSET;
        touch_y = touch_y < 0 ? 0 : (touch_y > screen_height - 1 ? screen_height - 1 : touch_y);
        if (stable_x < 0 || abs(touch_x - stable_x) >= 4 || abs(touch_y - stable_y) >= 4) {
            stable_x = touch_x;
            stable_y = touch_y;
        }
        data->point.x = stable_x;
        data->point.y = stable_y;
        data->state = LV_INDEV_STATE_PR;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

/* ================================================================
 * Brightness (persisted in NVS)
 * ================================================================ */
static void apply_brightness(int pct) {
    if (pct < BRIGHTNESS_MIN_PCT) pct = BRIGHTNESS_MIN_PCT;
    if (pct > 100) pct = 100;
    brightness_pct = pct;
    backlight_set(brightness_pct);
    nvs_set_i32(s_nvs, "bright", brightness_pct);
    nvs_commit(s_nvs);
}

/* ================================================================
 * UI — screens (identical composition to the sketch)
 * ================================================================ */
static lv_obj_t *new_screen(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

static void create_home_screen(void) {
    home_screen = new_screen();

    home_icon = lv_img_create(home_screen);
    lv_img_set_src(home_icon, &libremoto_icon);
    lv_img_set_antialias(home_icon, true);
    lv_obj_align(home_icon, LV_ALIGN_CENTER, 0, -60);

    ble_dot = lv_obj_create(home_screen);
    lv_obj_remove_style_all(ble_dot);
    lv_obj_set_size(ble_dot, 22, 22);
    lv_obj_set_style_radius(ble_dot, 11, 0);
    lv_obj_set_style_bg_color(ble_dot, C_WARN, 0);
    lv_obj_set_style_bg_opa(ble_dot, LV_OPA_COVER, 0);
    lv_obj_align(ble_dot, LV_ALIGN_CENTER, 0, 80);
}

static void create_map_screen(void) {
    map_screen = new_screen();
    /* opaque BLACK bg — DIRECT mode semantics (see sketch comment) */
    lv_obj_set_style_bg_color(map_screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(map_screen, LV_OPA_COVER, 0);

    if (g_map_fb) {
        for (size_t i = 0; i < (size_t)SCREEN_W * SCREEN_H; i++) g_map_fb[i] = 0;
        ESP_LOGI(TAG, "[MAP] full-frame renderer: DIRECT fb mode");
    } else {
        ESP_LOGE(TAG, "[MAP] FATAL: no PSRAM — map renderer needs the fb!");
    }

    /* All map-screen elements are painted into the frame by map_blit;
       the LVGL objects below exist only as hidden anchors (the sketch's
       verified architecture — never un-hide them). */
    map_vehicle = lv_img_create(map_screen);
    lv_img_set_src(map_vehicle, &vehicle_icon);
    lv_img_set_antialias(map_vehicle, true);
    lv_obj_set_pos(map_vehicle, MAP_CX - 32, MAP_CY - 32);
    lv_obj_remove_flag(map_vehicle, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_CHAIN));

    map_marker = lv_obj_create(map_screen);
    lv_obj_remove_style_all(map_marker);
    lv_obj_set_size(map_marker, 24, 24);
    lv_obj_set_style_radius(map_marker, 12, 0);
    lv_obj_set_style_bg_color(map_marker, C_ACCENT, 0);
    lv_obj_set_style_bg_opa(map_marker, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(map_marker, 3, 0);
    lv_obj_set_style_border_color(map_marker, C_TEXT, 0);
    lv_obj_remove_flag(map_marker, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_CHAIN));
    lv_obj_add_flag(map_marker, LV_OBJ_FLAG_HIDDEN);

    map_scale = lv_line_create(map_screen);
    static lv_point_precise_t scale_pts[2];
    scale_pts[0].x = 0;   scale_pts[0].y = 0;
    scale_pts[1].x = 100; scale_pts[1].y = 0;
    lv_line_set_points(map_scale, scale_pts, 2);
    lv_obj_set_size(map_scale, 100, 1);
    lv_obj_align(map_scale, LV_ALIGN_BOTTOM_LEFT, 24, -16);
    lv_obj_set_style_line_width(map_scale, 3, 0);
    lv_obj_set_style_line_color(map_scale, C_TEXT, 0);
    lv_obj_remove_flag(map_scale, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_CHAIN));
    lv_obj_add_flag(map_scale, LV_OBJ_FLAG_HIDDEN);

    map_label = lv_label_create(map_screen);
    lv_label_set_text(map_label, "Waiting for map data...");
    lv_obj_set_style_text_font(map_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(map_label, C_TEXT2, 0);
    lv_obj_align(map_label, LV_ALIGN_CENTER, 0, -215);
    lv_obj_remove_flag(map_label, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_CHAIN));
    lv_obj_add_flag(map_label, LV_OBJ_FLAG_HIDDEN);

    map_nav_panel = lv_obj_create(map_screen);
    lv_obj_remove_style_all(map_nav_panel);
    lv_obj_set_size(map_nav_panel, 480, 84);
    lv_obj_align(map_nav_panel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_radius(map_nav_panel, 0, 0);
    lv_obj_set_style_bg_color(map_nav_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(map_nav_panel, LV_OPA_60, 0);
    lv_obj_set_style_border_width(map_nav_panel, 0, 0);
    lv_obj_remove_flag(map_nav_panel, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_CHAIN));
    lv_obj_add_flag(map_nav_panel, LV_OBJ_FLAG_HIDDEN);

    map_nav_arrow = lv_img_create(map_screen);
    lv_img_set_src(map_nav_arrow, &maneuver_icon_straight);
    lv_img_set_antialias(map_nav_arrow, true);
    lv_obj_align(map_nav_arrow, LV_ALIGN_CENTER, -95, 0);
    lv_obj_remove_flag(map_nav_arrow, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_CHAIN));
    lv_obj_add_flag(map_nav_arrow, LV_OBJ_FLAG_HIDDEN);

    map_nav_dist = lv_label_create(map_screen);
    lv_label_set_text(map_nav_dist, "-");
    lv_obj_set_style_text_font(map_nav_dist, &lv_font_montserrat_34, 0);
    lv_obj_set_style_text_color(map_nav_dist, C_TEXT, 0);
    lv_obj_align_to(map_nav_dist, map_nav_arrow, LV_ALIGN_OUT_RIGHT_MID, 16, 0);
    lv_obj_remove_flag(map_nav_dist, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_CHAIN));
    lv_obj_add_flag(map_nav_dist, LV_OBJ_FLAG_HIDDEN);

    map_limit = lv_obj_create(map_screen);
    lv_obj_remove_style_all(map_limit);
    lv_obj_set_size(map_limit, 64, 64);
    lv_obj_set_style_radius(map_limit, 32, 0);
    lv_obj_set_style_bg_color(map_limit, C_TEXT, 0);
    lv_obj_set_style_bg_opa(map_limit, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(map_limit, 7, 0);
    lv_obj_set_style_border_color(map_limit, C_BAD, 0);
    lv_obj_set_pos(map_limit, 360 - 32, 120 - 32);
    lv_obj_remove_flag(map_limit, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_CHAIN));
    lv_obj_add_flag(map_limit, LV_OBJ_FLAG_HIDDEN);

    map_limit_label = lv_label_create(map_screen);
    lv_label_set_text(map_limit_label, "50");
    lv_obj_set_style_text_font(map_limit_label, &lv_font_montserrat_34, 0);
    lv_obj_add_flag(map_limit_label, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align_to(map_limit_label, map_limit, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(map_limit_label, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_CHAIN));
    lv_obj_add_flag(map_limit_label, LV_OBJ_FLAG_HIDDEN);
}

static void create_info_screen(void) {
    info_screen = new_screen();

    info_title = lv_label_create(info_screen);
    lv_label_set_text(info_title, "Arrived");
    lv_obj_set_style_text_font(info_title, &lv_font_montserrat_34, 0);
    lv_obj_align(info_title, LV_ALIGN_CENTER, 0, 0);

    info_sub = lv_label_create(info_screen);
    lv_label_set_text(info_sub, "Hold touch to go back");
    lv_obj_set_style_text_font(info_sub, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(info_sub, C_TEXT2, 0);
    lv_obj_align(info_sub, LV_ALIGN_CENTER, 0, 56);
}

/* ================================================================
 * NavData → UI / state machine (protocol.md §2 semantics)
 * ================================================================ */
static void render_nav_from_state(void) {
    /* band content is painted into the frame by render_map_display; the
       LVGL objects stay hidden (verified architecture). */
}

static void on_nav_state(const NavState *st) {
    g_nav = *st;
    have_nav = true;
    last_data_ms = lm_millis();
    rx_ok_count++;

    static bool reroute_hint = false;
    if (st->msg_type == NAV_MSG_REROUTE) {
        if (app != APP_NAV) {
            app = APP_NAV;
            if (!g_splash_until_ms) goto_screen(map_screen, "map (reroute)");
        }
        reroute_hint = !reroute_hint;
        if (reroute_hint) toast("Recalculating route...", C_WARN);
        return;
    }

    switch (st->msg_type) {
        case NAV_MSG_BEEP:
            break;
        case NAV_MSG_NAV:
            if (app != APP_NAV) {
                app = APP_NAV;
                if (!g_splash_until_ms) goto_screen(map_screen, "map (nav)");
            }
            break;
        case NAV_MSG_IDLE:
            app = APP_IDLE;
            state_until_ms = 0;
            if (!g_splash_until_ms) goto_screen(home_screen, "home (idle)");
            break;
        case NAV_MSG_ARRIVED:
            app = APP_ARRIVED;
            state_until_ms = lm_millis() + ARRIVED_TIMEOUT_MS;
            if (info_title) {
                lv_label_set_text(info_title, "Arrived");
                lv_obj_set_style_text_color(info_title, C_ACCENT, 0);
                lv_label_set_text(info_sub, "Hold touch to go back");
            }
            if (!g_splash_until_ms) goto_screen(info_screen, "arrived");
            break;
        default:
            break;
    }
}

/* ================================================================
 * State timeouts (protocol.md §2)
 * ================================================================ */
static void handle_state_timeouts(void) {
    uint32_t now = lm_millis();

    if (g_splash_until_ms && (int32_t)(now - g_splash_until_ms) >= 0) {
        g_splash_until_ms = 0;
        lv_obj_t *target = home_screen;
        const char *why = "home (splash done)";
        if (app == APP_NAV)          { target = map_screen;  why = "map (splash done, nav)"; }
        else if (app == APP_ARRIVED)  { target = info_screen; why = "info (splash done, arrived)"; }
        active_screen = target;
        lv_scr_load(target);
        ESP_LOGI(TAG, "[UI] -> %s", why);
    }

    if (have_nav && app == APP_NAV && (int32_t)(now - last_data_ms) > LINK_TIMEOUT_MS) {
        app = APP_LOST;
        state_until_ms = now + LOST_TIMEOUT_MS;
        if (info_title) {
            lv_label_set_text(info_title, "Connection lost");
            lv_obj_set_style_text_color(info_title, C_BAD, 0);
            lv_label_set_text(info_sub, "Waiting for phone...");
        }
        goto_screen(info_screen, "link lost");
        ESP_LOGI(TAG, "[STATE] NAV -> LOST (no data > 15 s)");
    }

    if ((app == APP_LOST || app == APP_ARRIVED) && state_until_ms &&
        (int32_t)(now - state_until_ms) >= 0) {
        const char *from = (app == APP_ARRIVED) ? "ARRIVED" : "LOST";
        app = APP_IDLE;
        state_until_ms = 0;
        goto_screen(home_screen, "home (timeout)");
        ESP_LOGI(TAG, "[STATE] %s -> IDLE (timeout)", from);
    }
}

/* ================================================================
 * Status payload (protocol.md §3)
 * ================================================================ */
static const char *app_state_name(AppState s) {
    switch (s) {
        case APP_NAV:     return "nav";
        case APP_IDLE:    return "idle";
        case APP_ARRIVED: return "arrived";
        case APP_LOST:    return "nav";
        default:          return "idle";
    }
}

static void publish_status(void) {
    static char payload[180];
    snprintf(payload, sizeof payload,
             "{\"v\":1,\"state\":\"%s\",\"ble_rx_count\":%u,"
             "\"brightness_pct\":%d,\"uptime_s\":%lu,"
             "\"seq_last\":%u,\"map_drop_count\":%u}",
             app_state_name(app),
             (unsigned)rx_count,
             brightness_pct,
             (unsigned long)(lm_millis() / 1000),
             (unsigned)map_seq_last,
             (unsigned)map_drop_count);
    if (ble_link_connected()) {
        ble_link_update_status(payload);
    }
}

/* ================================================================
 * BLE callbacks (COMM task context)
 * ================================================================ */
static void on_ble_nav(const void *data, size_t len) {
    char buf[NAV_MAX_LEN + 1];
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, data, len);
    buf[len] = 0;

    rx_count++;
    NavState st;
    if (parse_nav_message(buf, len, &st)) {
        ESP_LOGI(TAG, "[RX] BLE frame #%u OK  t=%d %u B  limit=%d dist=%d %s",
                 (unsigned)rx_count, (unsigned)st.msg_type, (unsigned)len,
                 st.limit_kmh, st.dist_m, st.maneuver);
        struct ui_msg m;
        m.kind = UI_MSG_NAV;
        m.nav = st;
        ui_push(&m);
    } else {
        ESP_LOGW(TAG, "[RX] BLE frame #%u PARSE ERROR (%u B)",
                 (unsigned)rx_count, (unsigned)len);
    }
    publish_status();
}

/* ---- dead reckoning (same design as the sketch) ---- */
static bool     g_dr_have = false;
static int32_t  g_dr_dy = 0;
static uint32_t g_dr_last_ms = 0;
static int32_t  g_dr_px_s = 0;
static int32_t  g_dr_dy_milli = 0;
#define DR_TICK_MS 75

static void dr_repaint(const MapRenderFrame *rf, int dy) {
    if (!g_panel_fb || !g_map_fb || !rf) return;
    map_blit_init(g_map_fb);
    map_blit_frame_dy(rf, dy);
    map_blit_band();
    map_blit_limit_sign(g_nav.limit_kmh > 0);
    if (rf->show_vehicle)
        map_blit_vehicle(vehicle_icon_map, MAP_CX, MAP_CY, 64);
    if (rf->marker_visible)
        map_blit_marker(rf->marker_x, rf->marker_y + dy);
    {
        char d[16];
        if (g_nav.dist_m >= 1000)
            snprintf(d, sizeof d, "%d.%d km", g_nav.dist_m / 1000, (g_nav.dist_m % 1000) / 100);
        else if (g_nav.dist_m > 0)
            snprintf(d, sizeof d, "%d m", g_nav.dist_m);
        else
            snprintf(d, sizeof d, "--");
        const int band_y = SCREEN_H - 84;
        const lv_font_t *f = &lv_font_montserrat_34;
        const int icon_w = 48, gap = 16;
        const int text_w = (int)(strlen(d) * (f->line_height / 2));
        const int group_w = icon_w + gap + text_w;
        const int gx0 = (SCREEN_W - group_w) / 2;
        map_blit_vehicle(maneuver_icon_map(g_nav.maneuver[0] ? g_nav.maneuver : "straight"),
                         gx0 + icon_w / 2, band_y + 42, 48);
        map_blit_text_lv((void *)f, d, gx0 + icon_w + gap,
                         band_y + 42 - f->line_height / 2, 0xFFFF);
    }
    if (g_nav.limit_kmh > 0) {
        char s[8];
        snprintf(s, sizeof s, "%d", g_nav.limit_kmh);
        const lv_font_t *f = (g_nav.limit_kmh >= 100) ? &lv_font_montserrat_24
                              : &lv_font_montserrat_34;
        const int w = (int)strlen(s) * (f->line_height / 2);
        map_blit_text_lv((void *)f, s, 360 - w / 2, 120 - f->line_height / 2, 0x0000);
    }
    memcpy(g_panel_fb, g_map_fb, (size_t)SCREEN_W * SCREEN_H * 2);
}

static void dr_tick_cb(lv_timer_t *t) {
    (void)t;
    if (!g_dr_have || active_screen != map_screen) return;
    if (g_nav.speed_kmh <= 0) return;
    if (g_map_mux == NULL) return;

    uint32_t now = lm_millis();
    if (!g_dr_last_ms) { g_dr_last_ms = now; return; }
    uint32_t dt = now - g_dr_last_ms;
    g_dr_last_ms = now;
    if (dt == 0 || dt > 500) return;

    xSemaphoreTake(g_map_mux, portMAX_DELAY);
    int32_t px_s_milli = (int32_t)(((int64_t)g_nav.speed_kmh * g_map_out.scale * 1000) / 360);
    g_dr_dy_milli += (int32_t)(((int64_t)px_s_milli * dt) / 1000);
    g_dr_px_s = px_s_milli / 1000;
    int32_t dy = g_dr_dy_milli / 1000;
    if (dy > 120 || dy < -30) { dy = 0; g_dr_dy_milli = 0; }
    dr_repaint(&g_map_out, dy);
    xSemaphoreGive(g_map_mux);
}

static void render_map_display(const MapRenderFrame *rf) {
    if (!g_panel_fb || !g_map_fb) {
        ESP_LOGW(TAG, "[MAP] render skipped: no fb!");
        return;
    }

    static uint32_t fps_frames = 0;
    static uint32_t fps_window_ms = 0;
    static uint32_t fps_last_ms = 0;
    {
        uint32_t now = lm_millis();
        if (fps_last_ms == 0) fps_last_ms = now;
        fps_frames++;
        fps_window_ms = now - fps_last_ms;
        if (fps_window_ms >= 10000) {
            ESP_LOGI(TAG, "[FPS] map: %u.%u fps (frame time %u ms avg)",
                     (unsigned)(fps_frames * 1000 / fps_window_ms),
                     (unsigned)((fps_frames * 10000 / fps_window_ms) % 10),
                     (unsigned)(fps_window_ms / fps_frames));
            fps_frames = 0;
            fps_last_ms = now;
        }
    }

    g_dr_have = true;
    g_dr_last_ms = 0;
    g_dr_dy = 0;
    g_dr_dy_milli = 0;

    map_blit_init(g_map_fb);
    map_blit_frame(rf);
    map_blit_band();
    map_blit_limit_sign(g_nav.limit_kmh > 0);
    if (rf->show_vehicle)
        map_blit_vehicle(vehicle_icon_map, MAP_CX, MAP_CY, 64);
    if (rf->marker_visible)
        map_blit_marker(rf->marker_x, rf->marker_y);

    if (g_nav.limit_kmh > 0) {
        char s[8];
        snprintf(s, sizeof s, "%d", g_nav.limit_kmh);
        const lv_font_t *f = (g_nav.limit_kmh >= 100) ? &lv_font_montserrat_24
                              : &lv_font_montserrat_34;
        const int w = (int)strlen(s) * (f->line_height / 2);
        map_blit_text_lv((void *)f, s, 360 - w / 2, 120 - f->line_height / 2, 0x0000);
    }
    {
        char d[16];
        if (g_nav.dist_m >= 1000) {
            snprintf(d, sizeof d, "%d.%d km", g_nav.dist_m / 1000,
                     (g_nav.dist_m % 1000) / 100);
        } else if (g_nav.dist_m > 0) {
            snprintf(d, sizeof d, "%d m", g_nav.dist_m);
        } else {
            snprintf(d, sizeof d, "--");
        }
        const int band_y = SCREEN_H - 84;
        const lv_font_t *f = &lv_font_montserrat_34;
        const int icon_w = 48, gap = 16;
        const int text_w = (int)(strlen(d) * (f->line_height / 2));
        const int group_w = icon_w + gap + text_w;
        const int gx0 = (SCREEN_W - group_w) / 2;
        map_blit_vehicle(maneuver_icon_map(g_nav.maneuver[0] ? g_nav.maneuver : "straight"),
                         gx0 + icon_w / 2, band_y + 42, 48);
        map_blit_text_lv((void *)f, d, gx0 + icon_w + gap,
                         band_y + 42 - f->line_height / 2, 0xFFFF);
    }
    memcpy(g_panel_fb, g_map_fb, (size_t)SCREEN_W * SCREEN_H * 2);

    /* everything is painted into the frame — LVGL objects stay hidden */
    if (map_vehicle) lv_obj_add_flag(map_vehicle, LV_OBJ_FLAG_HIDDEN);
    if (map_marker)  lv_obj_add_flag(map_marker, LV_OBJ_FLAG_HIDDEN);
    if (map_scale)   lv_obj_add_flag(map_scale, LV_OBJ_FLAG_HIDDEN);
    if (map_label)   lv_obj_add_flag(map_label, LV_OBJ_FLAG_HIDDEN);
}

static bool map_blit_if_new(void) {
    if (!g_map_ready || active_screen != map_screen) return false;
    if (g_map_mux == NULL) return false;
    xSemaphoreTake(g_map_mux, portMAX_DELAY);
    if (!g_map_ready) {
        xSemaphoreGive(g_map_mux);
        return false;
    }
    g_map_ready = false;
    render_map_display(&g_map_out);
    xSemaphoreGive(g_map_mux);
    return true;
}

static void on_ble_map_frame(const void *data, size_t len) {
    const uint8_t *f = (const uint8_t *)data;

    if (len > MAP_FRAME_MAX) {
        map_drop_count++;
        ESP_LOGW(TAG, "[MAP] drop: > 4 KB");
        publish_status();
        return;
    }

    static MapFrame fr;
    xSemaphoreTake(g_map_parse_mux, portMAX_DELAY);
    if (!parse_map_frame(f, len, &fr)) {
        xSemaphoreGive(g_map_parse_mux);
        map_drop_count++;
        ESP_LOGW(TAG, "[MAP] drop: parse error (%u B)", (unsigned)len);
        publish_status();
        return;
    }

    if (map_seq_seen) {
        int8_t d = (int8_t)(fr.seq - map_seq_last);
        if (d == 0) { xSemaphoreGive(g_map_parse_mux); return; }
        if (d < -64) {
            xSemaphoreGive(g_map_parse_mux);
            map_drop_count++;
            ESP_LOGW(TAG, "[MAP] drop: seq %u stale (last %u)", fr.seq, map_seq_last);
            publish_status();
            return;
        }
    }
    map_seq_last = fr.seq;
    map_seq_seen = true;
    ESP_LOGI(TAG, "[MAP] frame seq=%u accepted (%u B, %u segs, heading %d)",
             fr.seq, (unsigned)len, (unsigned)fr.seg_count, (int)fr.heading);

    xSemaphoreTake(g_map_mux, portMAX_DELAY);
    prepare_map_frame(&fr, &g_map_out);
    g_map_ready = true;
    xSemaphoreGive(g_map_mux);
    xSemaphoreGive(g_map_parse_mux);

    publish_status();
}

static void on_ble_ctrl(const void *data, size_t len) {
    char buf[CTRL_MAX_LEN + 1];
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, data, len);
    buf[len] = 0;

    CtrlState cs;
    if (!parse_ctrl_message(buf, len, &cs)) {
        ESP_LOGW(TAG, "[CTL] PARSE ERROR (%u B): %.60s", (unsigned)len, buf);
        publish_status();
        return;
    }
    if (cs.reset_map) {
        map_seq_last = 0;
        map_seq_seen = false;
        map_drop_count = 0;
        ESP_LOGI(TAG, "[CTL] map buffer reset (seq -> 0)");
    }
    if (cs.brightness_set || cs.force_idle) {
        struct ui_msg m;
        m.kind = UI_MSG_CTRL;
        m.ctrl = cs;
        ui_push(&m);
    }
    publish_status();
}

static void apply_ctrl(const CtrlState *cs) {
    if (cs->brightness_set) {
        apply_brightness(cs->brightness_pct);
        ESP_LOGI(TAG, "[CTL] brightness %d%%", cs->brightness_pct);
    }
    if (cs->force_idle) {
        app = APP_IDLE;
        state_until_ms = 0;
        goto_screen(home_screen, "home (ctrl)");
        ESP_LOGI(TAG, "[CTL] state -> idle");
    }
}

static void drain_ui_queue(void) {
    if (g_ui_mux == NULL) return;
    for (;;) {
        struct ui_msg m;
        xSemaphoreTake(g_ui_mux, portMAX_DELAY);
        if (ui_tail == ui_head) {
            xSemaphoreGive(g_ui_mux);
            break;
        }
        m = ui_q[ui_tail];
        ui_tail = (uint16_t)((ui_tail + 1) % UI_Q_SIZE);
        ui_free++;
        xSemaphoreGive(g_ui_mux);

        if (m.kind == UI_MSG_NAV) {
            on_nav_state(&m.nav);
            publish_status();
        } else if (m.kind == UI_MSG_CTRL) {
            apply_ctrl(&m.ctrl);
            publish_status();
        }
    }
}

/* ================================================================
 * BLE dot / toast
 * ================================================================ */
static void ble_blink_cb(lv_timer_t *t) {
    (void)t;
    static bool on = true;

    if (!ble_link_connected()) {
        on = !on;
        if (ble_dot) lv_obj_set_style_bg_color(ble_dot, on ? C_WARN : lv_color_hex(0x7A5A00), 0);
        return;
    }

    if (app == APP_IDLE) {
        on = !on;
        if (ble_dot) lv_obj_set_style_bg_color(ble_dot,
            on ? C_ACCENT : lv_color_hex(0x0A4A0F), 0);
    } else {
        if (ble_dot) lv_obj_set_style_bg_color(ble_dot, (app == APP_LOST) ? C_BAD : C_ACCENT, 0);
    }
}

static void toast_hide_cb(lv_timer_t *t) {
    (void)t;
    if (toast_label) lv_obj_del(toast_label);
    toast_label = NULL;
    lv_timer_del(toast_timer);
    toast_timer = NULL;
}

static void toast(const char *text, lv_color_t color) {
    if (toast_label) lv_obj_del(toast_label);
    toast_label = NULL;
    toast_label = lv_label_create(lv_screen_active());
    lv_obj_add_flag(toast_label, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_style_bg_color(toast_label, lv_color_hex(0x161B22), 0);
    lv_obj_set_style_bg_opa(toast_label, LV_OPA_90, 0);
    lv_obj_set_style_pad_hor(toast_label, 16, 0);
    lv_obj_set_style_pad_ver(toast_label, 10, 0);
    lv_obj_set_style_radius(toast_label, 10, 0);
    lv_obj_set_style_text_font(toast_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(toast_label, color, 0);
    lv_label_set_text(toast_label, text);
    lv_obj_center(toast_label);
    if (toast_timer) lv_timer_del(toast_timer);
    toast_timer = lv_timer_create(toast_hide_cb, 800, NULL);
}

/* ================================================================
 * MapData self-test (mt0..mt3) — same frames as the sketch
 * ================================================================ */
static void put_u16le(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void put_i16le(uint8_t *p, int16_t v)  { put_u16le(p, (uint16_t)v); }

static void map_put_seg(uint8_t **pp, uint8_t type, uint8_t width,
                        const int16_t *xy, int n) {
    uint8_t *p = *pp;
    p[0] = type; p[1] = width; p[2] = (uint8_t)n;
    p += 3;
    for (int i = 0; i < n; i++) {
        int16_t dx = (i == 0) ? xy[2*i]   : (int16_t)((int16_t)xy[2*i]   - xy[2*i-2]);
        int16_t dy = (i == 0) ? xy[2*i+1] : (int16_t)((int16_t)xy[2*i+1] - xy[2*i-1]);
        put_i16le(p, dx); p += 2; put_i16le(p, dy); p += 2;
    }
    *pp = p;
}

static void map_send_demo(uint8_t seq, uint8_t scenario, int16_t heading,
                          uint16_t flags, uint16_t scale) {
    static uint8_t buf[2048];
    uint8_t *p = buf;
    put_u16le(p, 0x4D4C); p += 2;
    *p++ = 1;
    *p++ = seq;
    put_u16le(p, flags); p += 2;
    put_i16le(p, heading); p += 2;
    put_u16le(p, scale); p += 2;

    if (scenario == 0) {
        *p++ = 2;
        static const int16_t road[8]  = {-200,-40, 200,-40};
        static const int16_t route[12] = {0,0, 0,-70, 0,-140, 0,-210, 0,-280};
        map_put_seg(&p, 2, 12, road,  2);
        map_put_seg(&p, 1, 10, route, 5);
    } else if (scenario == 1) {
        *p++ = 3;
        static const int16_t roadA[8]  = {-180,-120, 160,-200};
        static const int16_t route[10] = {0,0, -20,-60, -60,-120, -120,-170};
        static const int16_t branch[6] = {-60,-120, 80,-140};
        map_put_seg(&p, 2, 12, roadA,  2);
        map_put_seg(&p, 3, 8, branch, 2);
        map_put_seg(&p, 1, 10, route,  4);
    } else if (scenario == 2) {
        *p++ = 3;
        static const int16_t road1[6] = {60,-200, 60,220};
        static const int16_t road2[6] = {-140,-220, -140,240};
        static const int16_t route[14] = {0,0, 70,0, 140,0, 210,0, 280,0};
        map_put_seg(&p, 2, 12, road1, 2);
        map_put_seg(&p, 2, 12, road2, 2);
        map_put_seg(&p, 1, 10, route, 5);
    } else if (scenario == 3) {
        *p++ = 2;
        static const int16_t road[6] = {-220,0, 220,0};
        static const int16_t dest[4] = {0,0, 0,-160};
        map_put_seg(&p, 2, 16, road, 2);
        map_put_seg(&p, 4, 3, dest, 2);
    } else {
        *p++ = 1;
        static const int16_t route[8] = {0,0, 0,-100, 0,-200};
        map_put_seg(&p, 1, 10, route, 3);
    }

    size_t len = (size_t)(p - buf);
    ESP_LOGI(TAG, "[MT] demo scenario=%u seq=%u heading=%d flags=0x%04X scale=%u -> %u B",
             scenario, seq, (int)(heading/10), flags, scale, (unsigned)len);
    on_ble_map_frame(buf, len);
}

static void handle_map_test(const char *line) {
    int scenario = atoi(line + 2);
    const uint16_t ROT = 1u << 0;
    const uint16_t SCL = 1u << 1;
    const uint16_t VEH = 1u << 2;
    static uint8_t mt_seq = 0;
    uint8_t seq = mt_seq++;
    switch (scenario) {
        case 0: map_send_demo(seq, 0,   0, ROT|VEH, 20);  break;
        case 1: map_send_demo(seq, 1, 350, ROT|VEH, 20);  break;
        case 2: map_send_demo(seq, 2, 900, ROT|VEH, 20);  break;
        case 3: map_send_demo(seq, 3,   0, ROT|VEH|SCL, 50); break;
        default:
            ESP_LOGW(TAG, "[MT] unknown - use mt0 mt1 mt2 mt3");
            return;
    }
    if (active_screen != map_screen) goto_screen(map_screen, "map (self-test)");
}

/* ================================================================
 * Serial command dispatch (same as the sketch's handle_serial)
 * ================================================================ */
void serial_cmd_handle_line(const char *line) {
    if (line[0] == 'b' && (line[1] == 0 || (line[1] == '2' && line[2] == 0))) {
        /* benchmark: b = memcpy only, b2 = full render */
        const int N = 100;
        if (!g_panel_fb || !g_map_fb) { ESP_LOGW(TAG, "[BENCH] no fb"); return; }
        bool fullRender = (line[1] == '2');
        ESP_LOGI(TAG, "[BENCH] %s, %d iterations...",
                 fullRender ? "FULL RENDER" : "memcpy only", N);
        uint32_t t0 = lm_millis();
        for (int i = 0; i < N; i++) {
            if (fullRender) {
                map_blit_init(g_map_fb);
                map_blit_frame_dy(&g_map_out, (i % 40) - 20);
                map_blit_band();
                map_blit_limit_sign(g_nav.limit_kmh > 0);
                if (g_map_out.show_vehicle)
                    map_blit_vehicle(vehicle_icon_map, MAP_CX, MAP_CY, 64);
            }
            memcpy(g_panel_fb, g_map_fb, (size_t)SCREEN_W * SCREEN_H * 2);
        }
        uint32_t dt = lm_millis() - t0;
        ESP_LOGI(TAG, "[BENCH] %d full-screen updates in %lu ms = %.1f fps",
                 N, (unsigned long)dt, N * 1000.0 / dt);
        return;
    }
    if (line[0] == 'b') {
        apply_brightness(atoi(line + 1));
        ESP_LOGI(TAG, "[UI] brightness %d%%", brightness_pct);
        return;
    }
    if (line[0] == 'h') { goto_screen(home_screen, "home"); return; }
    if (line[0] == 'n') { goto_screen(map_screen, "map"); return; }
    if (line[0] == 'm' && line[1] == 't') { handle_map_test(line); return; }
    if (line[0] == 'm') { goto_screen(map_screen, "map"); return; }
    if (line[0] == '{') {
        rx_count++;
        NavState st;
        if (parse_nav_message(line, strlen(line), &st)) {
            ESP_LOGI(TAG, "[RX] frame #%u OK  t=%d %d B",
                     (unsigned)rx_count, (unsigned)st.msg_type, (unsigned)strlen(line));
            on_nav_state(&st);
        } else {
            ESP_LOGW(TAG, "[RX] frame #%u PARSE ERROR (%d B): %.60s",
                     (unsigned)rx_count, (unsigned)strlen(line), line);
        }
        return;
    }
}

static void goto_screen(lv_obj_t *scr, const char *name) {
    active_screen = scr;
    lv_scr_load(scr);
    ESP_LOGI(TAG, "[UI] -> %s", name);
}

/* ================================================================
 * Touch gestures (same thresholds as the sketch)
 * ================================================================ */
#define GESTURE_HOLD_MS     500
#define GESTURE_TAP_MAX_MS  300
#define GESTURE_SWIPE_PX    40

static bool g_touch_down = false;
static uint32_t g_touch_down_ms = 0;
static int16_t g_touch_start_x = 0, g_touch_start_y = 0;
static bool g_gesture_fired = false;

static void cycle_screen(void) {
    if (!active_screen) return;
    if (active_screen == home_screen) goto_screen(map_screen, "map (tap)");
    else                              goto_screen(home_screen, "home (tap)");
}

static void handle_gesture_state(void) {
    if (g_splash_until_ms) return;

    bool touching = touch_touched();

    if (touching && !g_touch_down) {
        int16_t px, py;
        touch_get_point(&px, &py);
        g_touch_down = true;
        g_gesture_fired = false;
        g_touch_down_ms = lm_millis();
        g_touch_start_x = px;
        g_touch_start_y = py;
        return;
    }

    if (touching && g_touch_down && !g_gesture_fired) {
        uint32_t held = lm_millis() - g_touch_down_ms;
        int16_t px, py;
        touch_get_point(&px, &py);
        int16_t dx = (int16_t)(px - g_touch_start_x);
        int16_t dy = (int16_t)(py - g_touch_start_y);
        int16_t ax = dx < 0 ? -dx : dx;
        int16_t ay = dy < 0 ? -dy : dy;

        if (held >= GESTURE_HOLD_MS) {
            if (active_screen != home_screen) {
                goto_screen(home_screen, "home (hold)");
                toast("Home", C_ACCENT);
            } else {
                toast("Home", C_ACCENT);
            }
            g_gesture_fired = true;
        } else if (held > 120 && ax >= GESTURE_SWIPE_PX && ax > ay) {
            if (!active_screen || active_screen == home_screen) {
                int delta = dx > 0 ? 10 : -10;
                apply_brightness(brightness_pct + delta);
                char buf[40];
                snprintf(buf, sizeof(buf), "Brightness %d%%", brightness_pct);
                toast(buf, C_TEXT);
            }
            g_gesture_fired = true;
        }
        return;
    }

    if (!touching && g_touch_down) {
        uint32_t held = lm_millis() - g_touch_down_ms;
        g_touch_down = false;
        if (!g_gesture_fired && held <= GESTURE_TAP_MAX_MS) {
            cycle_screen();
        }
    }
}

/* ================================================================
 * COMM task
 * ================================================================ */
static void comm_task(void *arg) {
    (void)arg;
    for (;;) {
        ble_link_poll();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/* ================================================================
 * Main
 * ================================================================ */
extern "C" void app_main(void) {
    ESP_LOGI(TAG, "[BOOT] ESP-IDF %s, LVGL %d.%d.%d, reset_reason=%d",
             esp_get_idf_version(),
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH,
             (int)esp_reset_reason());

    /* NVS (brightness persistence) */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(nvs_open("lm", NVS_READWRITE, &s_nvs));
    int32_t bright = 70;
    nvs_get_i32(s_nvs, "bright", &bright);
    brightness_pct = (int)bright;

    /* I2C bus (shared: PCF8574 + CST816) */
    i2c_master_bus_config_t i2c_cfg = {};
    i2c_cfg.i2c_port = I2C_NUM_0;
    i2c_cfg.sda_io_num = (gpio_num_t)I2C_SDA_PIN;
    i2c_cfg.scl_io_num = (gpio_num_t)I2C_SCL_PIN;
    i2c_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    i2c_cfg.glitch_ignore_cnt = 7;
    i2c_cfg.flags.enable_internal_pullup = true;
    i2c_master_bus_handle_t i2c_bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c_bus));

    /* Board power chain (expander sequencing, backlight) */
    board_power_init(i2c_bus);
    backlight_set(brightness_pct);

    /* Panel (ST7701 init + RGB peripheral; fb in PSRAM) */
    esp_lcd_panel_handle_t panel = NULL;
    uint16_t *fb = NULL;
    ESP_ERROR_CHECK(panel_init(&panel, &fb));
    g_panel_fb = fb;

    /* Touch */
    if (!touch_init(i2c_bus)) {
        ESP_LOGW(TAG, "touch not found - serial cmds still work");
    }

    /* LVGL */
    lv_init();
    lv_tick_set_cb(lm_millis);

    /* Map framebuffer (PSRAM offscreen; the panel fb is the DMA buffer) */
    size_t spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "[MEM] SPIRAM free: %u KB", (unsigned)(spiram_free / 1024));
    if (spiram_free == 0) {
        ESP_LOGE(TAG, "[MEM] NO SPIRAM! sdkconfig must enable octal PSRAM");
    }
    g_map_fb = (uint16_t *)heap_caps_malloc(
        sizeof(uint16_t) * SCREEN_W * SCREEN_H, MALLOC_CAP_SPIRAM);
    if (g_map_fb && map_blit_init(g_map_fb)) {
        for (size_t i = 0; i < (size_t)SCREEN_W * SCREEN_H; i++) g_map_fb[i] = 0;
        ESP_LOGI(TAG, "[MEM] map fb: PSRAM %u KB",
                 (unsigned)(SCREEN_W * SCREEN_H * 2 / 1024));
    } else {
        g_map_fb = NULL;
        ESP_LOGE(TAG, "[MEM] map fb alloc FAILED (wanted %u KB, SPIRAM free %u KB)",
                 (unsigned)(SCREEN_W * SCREEN_H * 2 / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }

    /* LVGL display: DIRECT mode over the PSRAM map fb (same as sketch) */
    if (g_map_fb) {
        lv_display_t *display = lv_display_create(screen_width, screen_height);
        lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
        lv_display_set_flush_cb(display, my_disp_flush);
        lv_display_set_buffers(display, (uint8_t *)g_map_fb, NULL,
                               (uint32_t)screen_width * screen_height * 2,
                               LV_DISPLAY_RENDER_MODE_DIRECT);

        lv_indev_t *touch_indev = lv_indev_create();
        lv_indev_set_type(touch_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(touch_indev, my_touchpad_read);
        lv_indev_set_display(touch_indev, display);
    } else {
        ESP_LOGE(TAG, "[FATAL] no map fb - LVGL DIRECT mode impossible");
    }

    /* Splash painted straight into the panel fb (NO LVGL render at boot).
     * Animated variant A "assembly" (website intro): the ticker runs from
     * the display loop while BLE init proceeds in parallel. */
    splash_anim_start();
    /* hold the finished logo for 1 s before the UI takes over */
    g_splash_until_ms = lm_millis() + splash_anim_total_ms + 1000;

    create_home_screen();
    create_map_screen();
    create_info_screen();

    /* Threading hand-off */
    g_map_mux = xSemaphoreCreateMutex();
    g_ui_mux = xSemaphoreCreateMutex();
    g_map_parse_mux = xSemaphoreCreateMutex();
    ui_free = UI_Q_SIZE;
    if (!g_map_mux || !g_ui_mux || !g_map_parse_mux) {
        ESP_LOGE(TAG, "[FATAL] hand-off mutex creation failed");
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* BLE GATT server */
    {
        struct ble_link_cb ble_cb = {0};
        ble_cb.on_nav = on_ble_nav;
        ble_cb.on_map_frame = on_ble_map_frame;
        ble_cb.on_ctrl = on_ble_ctrl;
        ble_link_init(&ble_cb);
    }

    /* COMM task */
    xTaskCreate(comm_task, "comm", 4096, NULL, 1, NULL);
    ESP_LOGI(TAG, "[TASK] comm task started (BLE + parse + geometry)");

    ble_timer = lv_timer_create(ble_blink_cb, 500, NULL);
    lv_timer_create(dr_tick_cb, DR_TICK_MS, NULL);

    serial_cmd_init();
    ESP_LOGI(TAG, "Libre-Moto ready. Gestures: tap=cycle, hold=home, swipe=brightness");
    ESP_LOGI(TAG, "Serial cmds: h n m | b<0-100> | mt0-3 | {\"v\":1,\"t\":...}");

    /* DISPLAY task main loop (app_main runs on the main task) */
    while (true) {
        serial_cmd_poll();
        drain_ui_queue();
        if (g_splash_until_ms) {
            if (!splash_anim_done()) splash_anim_tick(g_panel_fb);
            handle_state_timeouts();
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        map_blit_if_new();
        handle_gesture_state();
        handle_state_timeouts();
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
