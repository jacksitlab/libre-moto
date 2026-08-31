#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#include <esp_system.h>
#include <Adafruit_CST8XX.h>
#include <PCF8574.h>
#include <Preferences.h>
#include <math.h>
#include "config.h"
#include "../nav/nav_message.h" /* NavData + Control protocol v1 (protocol.md §2, §5) */
#include "../ble/ble_link.h"   /* GATT server: NavData/Status/MapData/Control         */
#include "../map/map_frame.h"  /* MapData binary frames (protocol.md §4)              */

/*---------------------------------------------------------------
 * Hardware (CrowPanel 2.1" — see docs/hardware.md)
 *--------------------------------------------------------------*/

PCF8574 pcf8574(PCF8574_ADDR);                 // I/O expander (reset/IRQ pins)
Adafruit_CST8XX ts_panel = Adafruit_CST8XX();   // CST816 capacitive touch

static const uint16_t screen_width  = SCREEN_W;
static const uint16_t screen_height = SCREEN_H;

/* ---- brightness (persisted) ---- */
Preferences prefs;
static int brightness_pct = 70;

/* ---- LVGL objects (screens + shared) ---- */
static uint8_t *buf1 = NULL;
static uint8_t *buf2 = NULL;

static lv_obj_t *home_screen = NULL;
static lv_obj_t *home_title = NULL;
static lv_obj_t *ble_dot = NULL;
static lv_obj_t *ble_label = NULL;
static lv_obj_t *home_msg = NULL;
static lv_obj_t *bright_bar = NULL;
static lv_obj_t *toast_label = NULL;
static lv_timer_t *toast_timer = NULL;

static lv_obj_t *nav_screen = NULL;
static lv_obj_t *nav_arrow = NULL;   // polyline, big maneuver arrow
static lv_obj_t *nav_dist = NULL;    // "350 m"
static lv_obj_t *nav_text = NULL;    // road name
static lv_obj_t *nav_status = NULL;  // "Link lost" / "Arrived" / ""

static lv_obj_t *map_screen = NULL;
/* segment pool: up to MAP_MAX_SEGS lines, created once, refilled per frame */
#define MAP_POOL      30
static lv_obj_t *map_lines[MAP_POOL];
static lv_obj_t *map_vehicle = NULL;   /* triangle: heading arrow (fixed) or marker (rotating) */
static lv_obj_t *map_marker = NULL;    /* destination marker (rotating frame) */
static lv_obj_t *map_scale = NULL;     /* scale bar line */
static lv_obj_t *map_label = NULL;     /* heading / scale label */

static lv_obj_t *active_screen = NULL;
static lv_timer_t *ble_timer = NULL;
static bool ble_dot_on = false;

/* ---- navigation state (from NavData, protocol.md §2) ---- */
static NavState    g_nav;            /* last valid parsed state            */
static bool        have_nav = false; /* at least one valid frame received  */
static uint32_t    rx_count = 0;     /* received frames (BLE-ready count)  */
static uint32_t    rx_ok_count = 0;   /* ... of which parsed OK             */
static unsigned long last_data_ms = 0;/* millis() of last valid frame       */
static AppState    app = APP_IDLE;

/* ---- MapFrame statistics (protocol.md §3/§4; render = C.1) ---- */
static uint8_t  map_seq_last = 0;     /* last OK frame seq     */
static uint32_t map_drop_count = 0;   /* dropped/stale/incomplete frames */
static bool     map_seq_seen = false; /* any OK frame yet?     */
static bool     ble_ready = false;    /* BLE link module up    */
static unsigned long state_until_ms = 0;

/* colors (dark theme, see docs/design.md) */
static const lv_color_t C_BG      = lv_color_hex(0x0D1117);
static const lv_color_t C_TEXT    = lv_color_hex(0xE6EDF3);
static const lv_color_t C_TEXT2   = lv_color_hex(0x8B949E);
static const lv_color_t C_ACCENT  = lv_color_hex(0x3FB950);
static const lv_color_t C_WARN    = lv_color_hex(0xD29922);
static const lv_color_t C_BAD     = lv_color_hex(0xF85149); /* lost / error (protocol.md §2, design.md) */
static lv_obj_t *info_screen = NULL;  /* S5 state overlay (arrived / link lost) */
static lv_obj_t *info_title = NULL;
static lv_obj_t *info_sub   = NULL;

/*---------------------------------------------------------------
 * RGB display driver configuration (ST7701S, type5 init)
 *--------------------------------------------------------------*/

Arduino_DataBus *panel_init_bus = new Arduino_SWSPI(
  GFX_NOT_DEFINED /* DC: ST7701 uses 9-bit SPI */, PIN_CS,
  PIN_SCK, PIN_SDA, GFX_NOT_DEFINED /* MISO */);

Arduino_ESP32RGBPanel *rgb_panel = new Arduino_ESP32RGBPanel(
  PIN_DE, PIN_VSYNC, PIN_HSYNC, PIN_PCLK,
  PIN_R0, PIN_R1, PIN_R2, PIN_R3, PIN_R4,
  PIN_G0, PIN_G1, PIN_G2, PIN_G3, PIN_G4, PIN_G5,
  PIN_B0, PIN_B1, PIN_B2, PIN_B3, PIN_B4,
  1 /* hsync polarity */, 10 /* hfp */, 4 /* hpw */, 20 /* hbp */,
  1 /* vsync polarity */, 10 /* vfp */, 4 /* vpw */, 20 /* vbp */,
  0 /* pclk active neg */, 12000000 /* pixel clock */, false /* native endian */,
  0 /* de idle high */, 0 /* pclk idle high */,
  480 * 20 /* two internal-DMA bounce buffers, 20 lines each */);

Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
  SCREEN_W, SCREEN_H, rgb_panel, 0 /* rotation */, true /* auto flush */,
  panel_init_bus, GFX_NOT_DEFINED /* RST (via PCF8574 P4) */,
  st7701_type5_init_operations, sizeof(st7701_type5_init_operations));

/*---------------------------------------------------------------
 * LVGL display output
 *--------------------------------------------------------------*/

void my_disp_flush(lv_display_t *display, const lv_area_t *area, uint8_t *px_map) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
  /* This ST7701 wiring needs the R/B 5-bit fields swapped. */
  uint16_t *pixels = (uint16_t *)px_map;
  const uint32_t pixel_count = w * h;
  for (uint32_t i = 0; i < pixel_count; ++i) {
    const uint16_t c = pixels[i];
    pixels[i] = (c & 0x07E0) | ((c & 0x001F) << 11) | ((c & 0xF800) >> 11);
  }
  gfx->draw16bitRGBBitmap(area->x1, area->y1, pixels, w, h);
  lv_display_flush_ready(display);
}

/*---------------------------------------------------------------
 * Touch input
 *--------------------------------------------------------------*/

void my_touchpad_read(lv_indev_t *indev, lv_indev_data_t *data) {
  (void)indev;
  if (ts_panel.touched()) {
    CST_TS_Point p = ts_panel.getPoint(0);
    /* Keep the last point for tiny moves so LVGL doesn't redraw a pressed
       object on every noisy sample. */
    static int16_t stable_x = -1;
    static int16_t stable_y = -1;
    const int16_t touch_x = constrain(p.x, 0, screen_width - 1);
    const int16_t touch_y = constrain(p.y - TOUCH_Y_OFFSET, 0, screen_height - 1);
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

/*---------------------------------------------------------------
 * Backlight
 *--------------------------------------------------------------*/

static void apply_brightness(int pct) {
  brightness_pct = constrain(pct, BRIGHTNESS_MIN_PCT, 100);
  ledcWrite(SCREEN_BACKLIGHT_PIN, (brightness_pct * 255) / 100);
  if (bright_bar) lv_bar_set_value(bright_bar, brightness_pct, LV_ANIM_OFF);
  /* Persist async-ish: NVS writes are cheap at 2 Hz */
  prefs.begin("lm", false);
  prefs.putInt("bright", brightness_pct);
  prefs.end();
}

/*---------------------------------------------------------------
 * UI — screen builders
 *--------------------------------------------------------------*/

static lv_obj_t *new_screen() {
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

  home_title = lv_label_create(home_screen);
  lv_label_set_text(home_title, "LibreMoto");
  lv_obj_set_style_text_font(home_title, &lv_font_montserrat_34, 0);
  lv_obj_set_style_text_color(home_title, C_TEXT, 0);
  lv_obj_align(home_title, LV_ALIGN_CENTER, 0, -120);

  ble_dot = lv_obj_create(home_screen);
  lv_obj_remove_style_all(ble_dot);
  lv_obj_set_size(ble_dot, 18, 18);
  lv_obj_set_style_radius(ble_dot, 9, 0);
  lv_obj_set_style_bg_color(ble_dot, C_WARN, 0);
  lv_obj_set_style_bg_opa(ble_dot, LV_OPA_COVER, 0);
  lv_obj_align(ble_dot, LV_ALIGN_CENTER, -90, -42);

  ble_label = lv_label_create(home_screen);
  lv_label_set_text(ble_label, "BLE: standby");
  lv_obj_set_style_text_font(ble_label, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(ble_label, C_TEXT2, 0);
  lv_obj_align(ble_label, LV_ALIGN_CENTER, 30, -42);

  home_msg = lv_label_create(home_screen);
  lv_label_set_text(home_msg, "Ready.\nTap: next screen  \u2022  Hold: home\nSwipe left/right: brightness");
  lv_obj_set_style_text_font(home_msg, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(home_msg, C_TEXT2, 0);
  lv_obj_align(home_msg, LV_ALIGN_CENTER, 0, 30);

  bright_bar = lv_bar_create(home_screen);
  lv_obj_set_size(bright_bar, 240, 14);
  lv_obj_set_style_bg_color(bright_bar, C_TEXT2, 0);
  lv_obj_set_style_bg_opa(bright_bar, LV_OPA_30, 0);
  lv_obj_set_style_radius(bright_bar, 7, 0);
  lv_obj_set_style_bg_color(bright_bar, C_ACCENT, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(bright_bar, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_align(bright_bar, LV_ALIGN_CENTER, 0, 110);
  lv_obj_remove_flag(bright_bar, LV_OBJ_FLAG_CLICKABLE);
  lv_bar_set_range(bright_bar, 0, 100);
  lv_bar_set_value(bright_bar, brightness_pct, LV_ANIM_OFF);
}

/* Maneuver arrow (lv_line, 120\u00d7120 box at object pos) */
static void set_arrow(const char *dir) {
  static lv_point_precise_t pts[8];
  int n;
  /* left: shaft + head pointing left    */
  static const lv_point_precise_t L[4] = {{112,60}, {12,60}, {12,25}, {12,95}};
  /* right: shaft + head pointing right  */
  static const lv_point_precise_t R[4] = {{8,60}, {108,60}, {108,25}, {108,95}};
  /* straight: up arrow                  */
  static const lv_point_precise_t S[3] = {{10,112}, {60,8}, {110,112}};
  /* U-turn (approx)                     */
  static const lv_point_precise_t U[4] = {{112,105}, {112,35}, {8,35}, {8,105}};
  const lv_point_precise_t *A;
  if      (!strcmp(dir, "left"))  { A = L; n = 4; }
  else if (!strcmp(dir, "right")) { A = R; n = 4; }
  else if (!strcmp(dir, "uturn")) { A = U; n = 4; }
  else                            { A = S; n = 3; }
  for (int i = 0; i < n; i++) pts[i] = A[i];
  lv_line_set_points(nav_arrow, pts, (uint32_t)n);
}

static void create_nav_screen(void) {
  nav_screen = new_screen();

  nav_arrow = lv_line_create(nav_screen);
  lv_obj_set_size(nav_arrow, 120, 120);
  lv_obj_align(nav_arrow, LV_ALIGN_CENTER, -170, 0);
  lv_obj_set_style_line_width(nav_arrow, 12, 0);
  lv_obj_set_style_line_color(nav_arrow, C_TEXT, 0);
  lv_obj_set_style_line_rounded(nav_arrow, true, 0);
  lv_obj_remove_flag(nav_arrow, LV_OBJ_FLAG_CLICKABLE);

  nav_dist = lv_label_create(nav_screen);
  lv_label_set_text(nav_dist, "350 m");
  lv_obj_set_style_text_font(nav_dist, &lv_font_montserrat_34, 0);
  lv_obj_set_style_text_color(nav_dist, C_TEXT, 0);
  lv_obj_align(nav_dist, LV_ALIGN_CENTER, 40, -20);
  lv_obj_remove_flag(nav_dist, LV_OBJ_FLAG_CLICKABLE);

  nav_text = lv_label_create(nav_screen);
  lv_label_set_text(nav_text, "Main St.");
  lv_obj_set_style_text_font(nav_text, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(nav_text, C_TEXT2, 0);
  lv_obj_align(nav_text, LV_ALIGN_CENTER, 40, 22);
  lv_obj_remove_flag(nav_text, LV_OBJ_FLAG_CLICKABLE);

  nav_status = lv_label_create(nav_screen);
  lv_label_set_text(nav_status, "");
  lv_obj_set_style_text_font(nav_status, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(nav_status, C_WARN, 0);
  lv_obj_align(nav_status, LV_ALIGN_CENTER, 0, 150);

  set_arrow("left");
}

static void create_map_screen(void) {
  map_screen = new_screen();

  /* ---- segment pool: MAP_POOL lines, created once ------------------
   * Per frame we only refill points[0..2] + size + visibility — no
   * object churn at 2 Hz. Points live in a static buffer per line. */
  /* shared static placeholder: the pool starts hidden, and every visible
     line gets its own valid map_pts[] pointer on the first frame.
     (A local array here would dangle once this function returns.) */
  static lv_point_precise_t map_pts_placeholder[1];
  map_pts_placeholder[0].x = 0;
  map_pts_placeholder[0].y = 0;
  for (int i = 0; i < MAP_POOL; i++) {
    map_lines[i] = lv_line_create(map_screen);
    lv_line_set_points(map_lines[i], map_pts_placeholder, 1);
    lv_obj_remove_flag(map_lines[i], (lv_obj_flag_t)((lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_CHAIN)));
    lv_obj_add_flag(map_lines[i], LV_OBJ_FLAG_HIDDEN);
    /* Z-order = creation order (LVGL 9): roads first, then the vehicle
       + marker created after the pool are drawn on top. */
  }

  /* ---- vehicle marker: white arrowhead "^" (chevron), tip pointing up ---- */
  map_vehicle = lv_line_create(map_screen);
  /* tip (240,212), wings (220,246) (260,246) — a bold "^" chevron.
     Points MUST live in a static buffer: lv_line keeps only the pointer
     and the array must outlive this function (stack would dangle). */
  static lv_point_precise_t vehicle_pts[3];
  vehicle_pts[0].x = 220; vehicle_pts[0].y = 246;
  vehicle_pts[1].x = 240; vehicle_pts[1].y = 212;
  vehicle_pts[2].x = 260; vehicle_pts[2].y = 246;
  lv_line_set_points(map_vehicle, vehicle_pts, 3);
  lv_obj_set_size(map_vehicle, 480, 480);   /* screen-absolute points */
  lv_obj_align(map_vehicle, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_style_line_width(map_vehicle, 10, 0);
  lv_obj_set_style_line_color(map_vehicle, C_TEXT, 0);  /* white */
  lv_obj_set_style_line_rounded(map_vehicle, true, 0);
  lv_obj_remove_flag(map_vehicle, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_CHAIN));
  lv_obj_remove_flag(map_vehicle, LV_OBJ_FLAG_HIDDEN);

  /* ---- destination dot marker (rotating frame only) ----------------- */
  map_marker = lv_obj_create(map_screen);
  lv_obj_remove_style_all(map_marker);
  lv_obj_set_size(map_marker, 24, 24);
  lv_obj_set_style_radius(map_marker, 12, 0);
  lv_obj_set_style_bg_color(map_marker, C_ACCENT, 0);
  lv_obj_set_style_bg_opa(map_marker, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(map_marker, 3, 0);
  lv_obj_set_style_border_color(map_marker, C_TEXT, 0);
  lv_obj_remove_flag(map_marker, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_CHAIN));
  lv_obj_add_flag(map_marker, LV_OBJ_FLAG_HIDDEN);   /* visible in rotating frames */

  /* ---- scale bar (bottom left; shown when MAP_FLAG_SCALE_BAR) ------- */
  map_scale = lv_line_create(map_screen);
  static lv_point_precise_t scale_pts[2];   /* static: lv_line keeps the pointer */
  scale_pts[0].x = 0;   scale_pts[0].y = 0;
  scale_pts[1].x = 100; scale_pts[1].y = 0;
  lv_line_set_points(map_scale, scale_pts, 2);
  lv_obj_set_size(map_scale, 100, 1);
  lv_obj_align(map_scale, LV_ALIGN_BOTTOM_LEFT, 24, -16);
  lv_obj_set_style_line_width(map_scale, 3, 0);
  lv_obj_set_style_line_color(map_scale, C_TEXT, 0);
  lv_obj_remove_flag(map_scale, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_CHAIN));
  lv_obj_add_flag(map_scale, LV_OBJ_FLAG_HIDDEN);

  /* ---- caption: heading + scale text ------------------------------- */
  map_label = lv_label_create(map_screen);
  lv_label_set_text(map_label, "Warte auf Karten-Daten…");
  lv_obj_set_style_text_font(map_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(map_label, C_TEXT2, 0);
  lv_obj_align(map_label, LV_ALIGN_CENTER, 0, -215);
  lv_obj_remove_flag(map_label, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_CHAIN));
}

/*---------------------------------------------------------------
 * S5 — info overlay screen (Arrived / Link lost; design.md)
 *--------------------------------------------------------------*/

static void create_info_screen(void) {
  info_screen = new_screen();

  info_title = lv_label_create(info_screen);
  lv_label_set_text(info_title, "Angekommen \u2713");
  lv_obj_set_style_text_font(info_title, &lv_font_montserrat_34, 0);
  lv_obj_align(info_title, LV_ALIGN_CENTER, 0, 0);

  info_sub = lv_label_create(info_screen);
  lv_label_set_text(info_sub, "Zur\u00fcck: Touch halten");
  lv_obj_set_style_text_font(info_sub, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(info_sub, C_TEXT2, 0);
  lv_obj_align(info_sub, LV_ALIGN_CENTER, 0, 56);
}

/*---------------------------------------------------------------
 * NavData → UI (protocol.md §2 semantics)
 *--------------------------------------------------------------*/

/* forward decls (defined later in the file) */
static void goto_screen(lv_obj_t *scr, const char *name);
static void toast(const char *text, lv_color_t color);
static void toast_debug(const char *text, lv_color_t color, uint32_t ms);

/* Render the current NavState on the nav screen (arrow + distance + road). */
static void render_nav_from_state(void) {
  set_arrow(g_nav.maneuver[0] ? g_nav.maneuver : "straight");
  char d[16];
  if (g_nav.dist_m > 0)      snprintf(d, sizeof d, "%d m", g_nav.dist_m);
  else if (g_nav.limit_kmh > 0) snprintf(d, sizeof d, "Limit %d", g_nav.limit_kmh);
  else                       strcpy(d, "\u2014");
  lv_label_set_text(nav_dist, d);

  const char *road = (g_nav.text[0] ? g_nav.text : "");
  lv_label_set_text(nav_text, road);
  lv_obj_set_style_text_color(nav_text, g_nav.text[0] ? C_TEXT2 : C_TEXT2, 0);

  if (g_nav.batt_pct >= 0) {
    char b[16];
    snprintf(b, sizeof b, "Bat %d%%", g_nav.batt_pct);
    /* show limit/batt in the status line if present, else free for "Reroute" */
    if (g_nav.limit_kmh > 0 && (g_nav.dist_m > 0 || !g_nav.text[0])) {
      char s[32];
      snprintf(s, sizeof s, "Limit %d km/h  \u2022  %s", g_nav.limit_kmh, b);
      lv_label_set_text(nav_status, s);
    } else {
      lv_label_set_text(nav_status, b);
    }
  }
}

/* Apply a freshly parsed NavState to the state machine + UI. */
static void on_nav_state(const NavState &st) {
  g_nav = st;
  have_nav = true;
  last_data_ms = millis();
  rx_ok_count++;

  /* Reroute: phone is recomputing — flash a status hint, stay on nav screen */
  static bool reroute_hint = false;
  if (st.msg_type == NAV_MSG_REROUTE) {
    if (app != APP_NAV) { app = APP_NAV; goto_screen(nav_screen, "nav (reroute)"); }
    reroute_hint = !reroute_hint;
    if (nav_status) {
      lv_label_set_text(nav_status, reroute_hint ? "Route wird berechnet…" : " ");
      lv_obj_set_style_text_color(nav_status, C_WARN, 0);
    }
    return;
  }

  switch (st.msg_type) {
    case NAV_MSG_BEEP:
      /* heartbeat: keeps the link alive; does not change state (protocol.md).
       * Toast doubles as the on-board BLE test aid (no state to change). */
      toast("Heartbeat ✓", C_ACCENT);
      break;

    case NAV_MSG_NAV: {
      if (app != APP_NAV) {
        app = APP_NAV;
        goto_screen(st.map_on ? map_screen : nav_screen, st.map_on ? "map (nav)" : "nav");
      }
      render_nav_from_state();
      break;
    }

    case NAV_MSG_IDLE:
      app = APP_IDLE;
      state_until_ms = 0;
      goto_screen(home_screen, "home (idle)");
      break;

    case NAV_MSG_ARRIVED:
      app = APP_ARRIVED;
      state_until_ms = millis() + ARRIVED_TIMEOUT_MS;
      if (info_title) {
        lv_label_set_text(info_title, "Angekommen ✓");
        lv_obj_set_style_text_color(info_title, C_ACCENT, 0);
        lv_label_set_text(info_sub, "Zurück: Touch halten");
      }
      goto_screen(info_screen, "arrived");
      break;

    default:
      break;
  }
}

/*---------------------------------------------------------------
 * State timeouts (protocol.md §2 semantics):
 *   NAV  + LINK_TIMEOUT_MS without valid frame   → LOST
 *   LOST + LOST_TIMEOUT_MS                       → IDLE
 *   ARRIVED + ARRIVED_TIMEOUT_MS                 → IDLE
 *--------------------------------------------------------------*/
static void handle_state_timeouts(void) {
  unsigned long now = millis();

  /* NAV → LOST: link silence while navigating */
  if (have_nav && app == APP_NAV && now - last_data_ms > LINK_TIMEOUT_MS) {
    app = APP_LOST;
    state_until_ms = now + LOST_TIMEOUT_MS;
    if (info_title) {
      lv_label_set_text(info_title, "Verbindung getrennt");
      lv_obj_set_style_text_color(info_title, C_BAD, 0);
      lv_label_set_text(info_sub, "Warte auf Telefon…");
    }
    goto_screen(info_screen, "link lost");
    Serial.println("[STATE] NAV → LOST (no data > 15 s)");
  }

  /* LOST / ARRIVED → IDLE after their display timeouts */
  if ((app == APP_LOST || app == APP_ARRIVED) && state_until_ms && now > state_until_ms) {
    const char *from = (app == APP_ARRIVED) ? "ARRIVED" : "LOST";
    app = APP_IDLE;
    state_until_ms = 0;
    goto_screen(home_screen, "home (timeout)");
    Serial.printf("[STATE] %s → IDLE (timeout)\n", from);
  }
}

/*---------------------------------------------------------------
 * Status payload (protocol.md §3) — device → phone JSON.
 * Rebuilt in place; sent via ble_link_update_status().
 *--------------------------------------------------------------*/
static const char *app_state_name(AppState s) {
  switch (s) {
    case APP_NAV:     return "nav";
    case APP_IDLE:    return "idle";
    case APP_ARRIVED: return "arrived";
    case APP_LOST:    return "nav"; /* protocol §3: state ∈ boot|idle|nav|arrived */
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
           (unsigned long)(millis() / 1000),
           (unsigned)map_seq_last,
           (unsigned)map_drop_count);
  if (ble_link_connected()) {
    ble_link_update_status(payload);
  }
}

/*---------------------------------------------------------------
 * BLE → app callbacks (run on the ARDUINO core via ble_link_poll).
 *--------------------------------------------------------------*/

/* NavData JSON (protocol.md §2) — same path as the serial mock. */
static void on_ble_nav(const void *data, size_t len) {
  char buf[NAV_MAX_LEN + 1];
  if (len >= sizeof(buf)) len = sizeof(buf) - 1;
  memcpy(buf, data, len);
  buf[len] = 0;

  rx_count++;
  NavState st;
  if (parse_nav_message(buf, len, &st)) {
    Serial.printf("[RX] BLE frame #%u OK  t=%d %u B\n",
                  (unsigned)rx_count, (unsigned)st.msg_type, (unsigned)len);
    on_nav_state(st);
    publish_status();
  } else {
    /* spec §6: parse error → frame ignored, rx_count still incremented.
     * On-board: show length + hex dump so we can read it without a wire. */
    Serial.printf("[RX] BLE frame #%u PARSE ERROR (%u B)\n",
                  (unsigned)rx_count, (unsigned)len);
    char dbg[96];
    const uint8_t *b = (const uint8_t *)buf;
    int n = len < 28 ? (int)len : 28;
    int p = 0;
    for (int i = 0; i < n && p < 80; i++)
      p += snprintf(dbg + p, sizeof(dbg) - p, "%02X ", b[i]);
    toast_debug(dbg, C_BAD, 4000);
    publish_status();
  }
}

/* MapData binary frame (protocol.md §4/§6) — decode + render.
 * Runs on the app core (ble_link_poll drains the queue). */

/* per-line point buffers: mutable so lv_line keeps our pointer (no malloc at 2 Hz) */
static lv_point_precise_t map_pts[MAP_POOL][MAP_MAX_PTS];

static void render_map_frame(const MapFrame *fr) {
  /* Rotate the world so that the vehicle heading points "up" (screen -y).
     heading unit = degrees × 10, 0° = north.  Screen: +x right, +y down. */
  double theta = fr->heading * 0.1 * (3.14159265358979323846 / 180.0);
  double ct = cos(theta), st = sin(theta);

  bool show_vehicle = (fr->flags & MAP_FLAG_VEHICLE) != 0;

  int used = 0;

  /* Z-order = creation order (LVGL 9): roads FIRST, route LAST (top).
     Roads/branches: two thin (2 px) bright edge lines offset ±width/2
     perpendicular to the center line (frame width = total road width).
     Route: single fat white line with rounded joins ("filled" look).  */

  /* pool budget: 1 slot per route/destination, 2 per road/branch */
  int n_route = 0;
  for (int si = 0; si < fr->seg_count; si++)
    if (fr->segs[si].type == MAP_SEG_ROUTE) n_route++;
  int route_room = n_route + 2;                 /* +2: destination + margin */
  if (route_room > MAP_POOL) route_room = MAP_POOL;
  int road_room = (MAP_POOL - route_room) / 2;
  if (road_room < 0) road_room = 0;

  static double base[2][MAP_MAX_PTS * 2]; /* base[0] center pts, base[1] edge pts */
  int road_i = 0;

  /* ---- pass 1a: roads / branches → 2 edge lines each ---------------- */
  for (int si = 0; si < fr->seg_count; si++) {
    const MapSeg *sg = &fr->segs[si];
    if (sg->type != MAP_SEG_ROAD && sg->type != MAP_SEG_BRANCH) continue;
    if (road_i >= road_room) continue;         /* pool exhausted — skip */
    road_i++;

    int n = sg->npts;
    for (int i = 0; i < n; i++) {
      double x = sg->points[i * 2 + 0];
      double y = sg->points[i * 2 + 1];
      base[0][i * 2 + 0] =  x * ct + y * st + 240.0;
      base[0][i * 2 + 1] = -x * st + y * ct + 240.0;
    }

    double half = sg->width * 0.5;
    if (half < 1.5) half = 1.5;

    for (int e = 0; e < 2 && used < MAP_POOL - 2 - n_route; e++) {
      double sign = (e == 0) ? 1.0 : -1.0;
      /* per-vertex offset along the average of neighboring normals so
         corners don't spike */
      for (int i = 0; i < n; i++) {
        double x = base[0][i * 2], y = base[0][i * 2 + 1];
        double nx = 0, ny = 0;
        if (n > 1) {
          if (i < n - 1) {
            double dx = base[0][(i + 1) * 2] - x, dy = base[0][(i + 1) * 2 + 1] - y;
            double L = sqrt(dx * dx + dy * dy);
            if (L > 0.0001) { nx += -dy / L; ny += dx / L; }
          }
          if (i > 0) {
            double dx = x - base[0][(i - 1) * 2], dy = y - base[0][(i - 1) * 2 + 1];
            double L = sqrt(dx * dx + dy * dy);
            if (L > 0.0001) { nx += -dy / L; ny += dx / L; }
          }
        }
        double L = sqrt(nx * nx + ny * ny);
        if (L < 0.0001) { nx = 0; ny = 1; } else { nx /= L; ny /= L; }
        double off = half * sign;
        base[1][i * 2 + 0] = x + nx * off;
        base[1][i * 2 + 1] = y + ny * off;
      }
      lv_obj_t *line = map_lines[used];
      for (int i = 0; i < n; i++) {
        map_pts[used][i].x = (lv_value_precise_t)lrint(base[1][i * 2 + 0]);
        map_pts[used][i].y = (lv_value_precise_t)lrint(base[1][i * 2 + 1]);
      }
      lv_line_set_points(line, map_pts[used], n);
      lv_obj_set_size(line, 480, 480);   /* points line-local → anchor at (0,0) */
      lv_obj_align(line, LV_ALIGN_TOP_LEFT, 0, 0);
      lv_obj_set_style_line_width(line, 2, 0);          /* thin edge */
      lv_obj_set_style_line_color(line, C_TEXT, 0);     /* bright white */
      lv_obj_set_style_line_rounded(line, true, 0);
      lv_obj_remove_flag(line, LV_OBJ_FLAG_HIDDEN);
      used++;
    }
  }

  /* ---- pass 1b: destination (accent, single line) ------------------- */
  for (int si = 0; si < fr->seg_count && used < MAP_POOL - n_route; si++) {
    const MapSeg *sg = &fr->segs[si];
    if (sg->type != MAP_SEG_DESTINATION) continue;
    lv_obj_t *line = map_lines[used];
    for (int i = 0; i < sg->npts; i++) {
      double x = sg->points[i * 2 + 0];
      double y = sg->points[i * 2 + 1];
      map_pts[used][i].x = (lv_value_precise_t)lrint( x * ct + y * st + 240.0);
      map_pts[used][i].y = (lv_value_precise_t)lrint(-x * st + y * ct + 240.0);
    }
    lv_line_set_points(line, map_pts[used], sg->npts);
    lv_obj_set_size(line, 480, 480);
    lv_obj_align(line, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_line_width(line, 4, 0);
    lv_obj_set_style_line_color(line, C_ACCENT, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_HIDDEN);
    used++;
  }

  /* ---- pass 2: route (topmost, fat white, rounded) ------------------ */
  for (int si = 0; si < fr->seg_count && used < MAP_POOL; si++) {
    const MapSeg *sg = &fr->segs[si];
    if (sg->type != MAP_SEG_ROUTE) continue;
    lv_obj_t *line = map_lines[used];
    for (int i = 0; i < sg->npts; i++) {
      double x = sg->points[i * 2 + 0];
      double y = sg->points[i * 2 + 1];
      map_pts[used][i].x = (lv_value_precise_t)lrint( x * ct + y * st + 240.0);
      map_pts[used][i].y = (lv_value_precise_t)lrint(-x * st + y * ct + 240.0);
    }
    lv_line_set_points(line, map_pts[used], sg->npts);
    lv_obj_set_size(line, 480, 480);
    lv_obj_align(line, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_line_width(line, sg->width, 0);   /* fat filled look */
    lv_obj_set_style_line_color(line, C_TEXT, 0);      /* white          */
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_HIDDEN);
    used++;
  }
  for (int i = used; i < MAP_POOL; i++)
    lv_obj_add_flag(map_lines[i], LV_OBJ_FLAG_HIDDEN);

  /* ---- destination dot marker (end point of destination segment) --- */
  if (map_marker) {
    lv_obj_add_flag(map_marker, LV_OBJ_FLAG_HIDDEN);
    for (int si = 0; si < fr->seg_count; si++) {
      if (fr->segs[si].type != MAP_SEG_DESTINATION) continue;
      int last = fr->segs[si].npts - 1;
      double x = fr->segs[si].points[last * 2 + 0];
      double y = fr->segs[si].points[last * 2 + 1];
      double rx =  x * ct + y * st;
      double ry = -x * st + y * ct;
      lv_coord_t ex = (lv_coord_t)lrint(rx + 240.0);
      lv_coord_t ey = (lv_coord_t)lrint(ry + 240.0);
      lv_obj_remove_flag(map_marker, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_pos(map_marker, ex - 12, ey - 12);
      break;
    }
  }

  /* ---- vehicle marker --------------------------------------------- */
  if (map_vehicle) {
    if (!show_vehicle) {
      lv_obj_add_flag(map_vehicle, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_remove_flag(map_vehicle, LV_OBJ_FLAG_HIDDEN);
      /* The world is already rotated onto the screen; the vehicle
         marker sits at screen centre and stays as-is. */
    }
  }

  /* ---- scale bar + caption ---------------------------------------- */
  bool show_scale = (fr->flags & MAP_FLAG_SCALE_BAR) != 0 && fr->scale > 0;
  if (show_scale) {
    lv_obj_remove_flag(map_scale, LV_OBJ_FLAG_HIDDEN);
    int meters = (int)((100 * 100) / fr->scale);
    if (meters < 1) meters = 1;
  } else {
    lv_obj_add_flag(map_scale, LV_OBJ_FLAG_HIDDEN);
  }

  if (map_label) {
    char txt[64];
    int heading_deg = (int)(fr->heading / 10);
    if (show_scale) {
      int meters = (int)((100 * 100) / fr->scale);
      snprintf(txt, sizeof txt, "%d°   |   %d m", heading_deg, meters);
    } else {
      snprintf(txt, sizeof txt, "%d°   |   %u Seg.", heading_deg, (unsigned)fr->seg_count);
    }
    lv_label_set_text(map_label, txt);
    lv_obj_remove_flag(map_label, LV_OBJ_FLAG_HIDDEN);
  }
}

static void on_ble_map_frame(const void *data, size_t len) {
  const uint8_t *f = (const uint8_t *)data;

  if (len > MAP_FRAME_MAX) {
    map_drop_count++;
    Serial.println("[MAP] drop: > 4 KB");
    publish_status();
    return;
  }

  /* decode first (bounds-checks all bytes, returns false on any error) */
  static MapFrame fr;   /* ~7 KB — lives in .bss, off the app-task stack */
  if (!parse_map_frame(f, len, &fr)) {
    map_drop_count++;
    Serial.printf("[MAP] drop: parse error (%u B)\n", (unsigned)len);
    publish_status();
    return;
  }

  /* seq handling (spec §6): duplicate ignored, >64 older = stale */
  if (map_seq_seen) {
    int8_t d = (int8_t)(fr.seq - map_seq_last);
    if (d == 0) return;              /* duplicate — no log, no counter */
    if (d < -64) {
      map_drop_count++;
      Serial.printf("[MAP] drop: seq %u stale (last %u)\n", fr.seq, map_seq_last);
      publish_status();
      return;
    }
  }
  map_seq_last = fr.seq;
  map_seq_seen = true;
  Serial.printf("[MAP] frame seq=%u accepted (%u B, %u segs, heading %d)\n",
                fr.seq, (unsigned)len, (unsigned)fr.seg_count, (int)fr.heading);

  /* render — only when the map screen is up (frames can arrive any time) */
  if (active_screen == map_screen) render_map_frame(&fr);

  publish_status();
}

/* Control JSON (protocol.md §5) */
static void on_ble_ctrl(const void *data, size_t len) {
  char buf[CTRL_MAX_LEN + 1];
  if (len >= sizeof(buf)) len = sizeof(buf) - 1;
  memcpy(buf, data, len);
  buf[len] = 0;

  CtrlState cs;
  if (!parse_ctrl_message(buf, len, &cs)) {
    Serial.printf("[CTL] PARSE ERROR (%u B): %.60s\n", (unsigned)len, buf);
    publish_status();
    return;
  }
  if (cs.brightness_set) {
    apply_brightness(cs.brightness_pct);   /* also persisted (spec §5) */
    Serial.printf("[CTL] brightness %d%%\n", cs.brightness_pct);
  }
  if (cs.force_idle) {
    app = APP_IDLE;
    state_until_ms = 0;
    goto_screen(home_screen, "home (ctrl)");
    Serial.println("[CTL] state → idle");
  }
  if (cs.reset_map) {
    map_seq_last = 0;
    map_seq_seen = false;
    map_drop_count = 0;
    Serial.println("[CTL] map buffer reset (seq → 0)");
  }
  publish_status();
}

/*---------------------------------------------------------------
 * BLE status dot: reflects the radio link AND the app state.
 *   Not connected   → grey,  steady ("BLE: offline")
 *   Connected + IDLE→ amber, 500 ms blink ("BLE: standby")
 *   Connected + NAV → green,  steady   ("BLE: connected ✓")
 *   Connected + LOST→ red,    steady   (link alive, nav route lost)
 * Dot lives on the home + nav screens; the 500 ms timer keeps the
 * colour right on whichever screen is active.
 *--------------------------------------------------------------*/

static void ble_blink_cb(lv_timer_t *t) {
  (void)t;
  static bool on = true;

  if (!ble_link_connected()) {
    if (ble_dot)   lv_obj_set_style_bg_color(ble_dot, lv_color_hex(0x3B4148), 0);
    if (ble_label) lv_label_set_text(ble_label, "BLE: offline");
    return;
  }

  if (app == APP_IDLE) {
    on = !on;
    if (ble_dot)   lv_obj_set_style_bg_color(ble_dot, on ? C_WARN : lv_color_hex(0x3B4148), 0);
    if (ble_label) lv_label_set_text(ble_label, "BLE: standby");
  } else {
    if (ble_dot)   lv_obj_set_style_bg_color(ble_dot, (app == APP_LOST) ? C_BAD : C_ACCENT, 0);
    if (ble_label) {
      if (app == APP_LOST)         lv_label_set_text(ble_label, "BLE: link lost");
      else if (app == APP_ARRIVED) lv_label_set_text(ble_label, "BLE: connected \u2713");
      else                         lv_label_set_text(ble_label, "BLE: connected \u2713");
    }
  }
}

/*---------------------------------------------------------------
 * Toast (short feedback overlay, auto-hidden)
 *--------------------------------------------------------------*/

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

/* Same, but small font + long lifetime — for hex dumps / diagnostics. */
static void toast_debug(const char *text, lv_color_t color, uint32_t ms) {
  if (toast_label) lv_obj_del(toast_label);
  toast_label = NULL;
  toast_label = lv_label_create(lv_screen_active());
  lv_obj_add_flag(toast_label, LV_OBJ_FLAG_FLOATING);
  lv_obj_set_style_bg_color(toast_label, lv_color_hex(0x161B22), 0);
  lv_obj_set_style_bg_opa(toast_label, LV_OPA_90, 0);
  lv_obj_set_style_pad_hor(toast_label, 12, 0);
  lv_obj_set_style_pad_ver(toast_label, 8, 0);
  lv_obj_set_style_radius(toast_label, 10, 0);
  lv_obj_set_style_text_font(toast_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(toast_label, color, 0);
  lv_obj_set_width(toast_label, 440);
  lv_obj_set_style_text_align(toast_label, LV_TEXT_ALIGN_LEFT, 0);
  lv_label_set_text(toast_label, text);
  lv_obj_align(toast_label, LV_ALIGN_CENTER, 0, 0);
  if (toast_timer) lv_timer_del(toast_timer);
  toast_timer = lv_timer_create(toast_hide_cb, ms, NULL);
}

/*---------------------------------------------------------------
 * MapData self-test generator (C.1c)
 *   Serial commands:
 *     mt0   frame 0 — heading 0°,  route straight ahead + cross road
 *     mt1   frame 1 — heading 35° (left-ish turn), route curves
 *     mt2   frame 2 — heading 90° (east), long straight
 *     mt3   frame 3 — scale bar on, 4 px/m, destination marker
 *   Each command builds a binary frame and pushes it through the
 *   REAL firmware path (on_ble_map_frame → parse → render → screen).
 *   No phone, no BLE connection needed.
 *--------------------------------------------------------------*/

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
  put_u16le(p, 0x4D4C); p += 2;                 /* magic */
  *p++ = 1;                                     /* ver   */
  *p++ = seq;                                   /* seq   */
  put_u16le(p, flags); p += 2;                  /* flags */
  put_i16le(p, heading); p += 2;                /* heading (deg × 10) */
  put_u16le(p, scale); p += 2;                  /* px_per_m × 100    */

  /* segment plan depends on the scenario (NOT on seq — see §6) */
  if (scenario == 0) {
    /* heading 0°: route straight ahead (world -y), cross road east–west */
    *p++ = 2;                                   /* 2 segments */
    static const int16_t road[8]  = {-200,-40, 200,-40};             /* y=-40 */
    static const int16_t route[12] = {0,0, 0,-70, 0,-140, 0,-210, 0,-280};
    map_put_seg(&p, 2, 12, road,  2);           /* road, 12 px total */
    map_put_seg(&p, 1, 10, route, 5);           /* route, 10 px      */
  } else if (scenario == 1) {
    /* heading 35°, route curving ahead-left, one branch */
    *p++ = 3;                                   /* 3 segments */
    static const int16_t roadA[8]  = {-180,-120, 160,-200};
    static const int16_t route[10] = {0,0, -20,-60, -60,-120, -120,-170};
    static const int16_t branch[6] = {-60,-120, 80,-140};
    map_put_seg(&p, 2, 12, roadA,  2);
    map_put_seg(&p, 3, 8, branch, 2);
    map_put_seg(&p, 1, 10, route,  4);
  } else if (scenario == 2) {
    /* heading 90° (east): long straight, two parallel cross roads */
    *p++ = 3;                                   /* 3 segments */
    static const int16_t road1[6] = {60,-200, 60,220};
    static const int16_t road2[6] = {-140,-220, -140,240};
    static const int16_t route[14] = {0,0, 70,0, 140,0, 210,0, 280,0};
    map_put_seg(&p, 2, 12, road1, 2);
    map_put_seg(&p, 2, 12, road2, 2);
    map_put_seg(&p, 1, 10, route, 5);
  } else if (scenario == 3) {
    /* scale-bar demo + destination dot ahead */
    *p++ = 2;                                   /* 2 segments */
    static const int16_t road[6] = {-220,0, 220,0};
    static const int16_t dest[4] = {0,0, 0,-160};
    map_put_seg(&p, 2, 16, road, 2);
    map_put_seg(&p, 4, 3, dest, 2);             /* destination */
  } else {
    /* fallback: single straight route */
    *p++ = 1;                                   /* 1 segment */
    static const int16_t route[8] = {0,0, 0,-100, 0,-200};
    map_put_seg(&p, 1, 10, route, 3);
  }

  size_t len = (size_t)(p - buf);
  Serial.printf("[MT] demo scenario=%u seq=%u heading=%d° flags=0x%04X scale=%u → %u B\n",
                scenario, seq, (int)(heading/10), flags, scale, (unsigned)len);
  on_ble_map_frame(buf, len);
}

static void handle_map_test(const char *line) {
  int scenario = atoi(line + 2);   /* "mt<0-3>" → scenario 0..3 */
  const uint16_t ROT  = 1u << 0;
  const uint16_t SCL  = 1u << 1;
  const uint16_t VEH  = 1u << 2;
  /* monotonic seq: a duplicate seq is a no-op per protocol §6, so the
     self-test must keep counting (also proves multi-frame rendering). */
  static uint8_t mt_seq = 0;
  uint8_t seq = mt_seq++;
  switch (scenario) {
    case 0: map_send_demo(seq, 0,   0, ROT|VEH, 20);  break;
    case 1: map_send_demo(seq, 1, 350, ROT|VEH, 20);  break;
    case 2: map_send_demo(seq, 2, 900, ROT|VEH, 20);  break;
    case 3: map_send_demo(seq, 3,   0, ROT|VEH|SCL, 50); break;
    default:
      Serial.println("[MT] unknown — use mt0 mt1 mt2 mt3"); return;
  }
  if (active_screen != map_screen) goto_screen(map_screen, "map (self-test)");
}

/*---------------------------------------------------------------
 * Serial debug channel (until BLE + phone app are in)
 *   h / n / m              → home / nav / map (quick jump)
 *   b<0-100>               → brightness
 *   mt0..mt3               → MapData self-test frames (C.1c)
 *   {"v":1,"t":...}        → NavData frame (protocol.md §2),
 *                            e.g. {"v":1,"t":"nav","maneuver":"left",
 *                                  "dist_m":350,"text":"Main St."}
 *   parse errors           → logged with the offending line (ignored)
 *--------------------------------------------------------------*/

static void goto_screen(lv_obj_t *scr, const char *name) {
  active_screen = scr;
  lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
  Serial.printf("[UI] -> %s\n", name);
}

/* Serial input line buffer (nav frames can exceed 100 bytes — generous budget) */
#define SER_LINE_MAX 512

static void handle_serial(void) {
  static char line[SER_LINE_MAX];
  static int li = 0;

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (li == 0) continue;
      line[li] = 0;
      /* trim trailing whitespace */
      while (li > 0 && (line[li-1] == ' ' || line[li-1] == '\t')) line[--li] = 0;
      li = 0;  /* reset for the next line */
      if (!line[0]) continue;

      if (line[0] == 'h')                 goto_screen(home_screen, "home");
      else if (line[0] == 'n')            goto_screen(nav_screen, "nav");
      else if (line[0] == 'm' && line[1] == 't') {
        handle_map_test(line);            /* 'mt' must precede the 'm' test */
      }
      else if (line[0] == 'm')            goto_screen(map_screen, "map");
      else if (line[0] == 'b') {
        apply_brightness(atoi(line + 1));
        Serial.printf("[UI] brightness %d%%\n", brightness_pct);
      }
      else if (line[0] == '{') {
        /* ---- NavData JSON frame (protocol.md §2) ---- */
        rx_count++;
        NavState st;
        if (parse_nav_message(line, strlen(line), &st)) {
          Serial.printf("[RX] frame #%u OK  t=%d %d B\n",
                        (unsigned)rx_count, (unsigned)st.msg_type, (unsigned)strlen(line));
          on_nav_state(st);
        } else {
          Serial.printf("[RX] frame #%u PARSE ERROR (%d B): %.60s\n",
                        (unsigned)rx_count, (unsigned)strlen(line), line);
        }
      }
    } else if (li < SER_LINE_MAX - 1) {
      line[li++] = c;
    }
  }
}

/*---------------------------------------------------------------
 * Touch gestures (NO rotary encoder \u2014 all input via touch):
 *   Tap    (< 300 ms)        \u2192 cycle  home \u2192 nav \u2192 map
 *   Hold   (\u2265 500 ms)       \u2192 back to home
 *   Swipe  (\u2265 40 px horiz) \u2192 brightness \u2212 / +  (only on home)
 *--------------------------------------------------------------*/
#define GESTURE_HOLD_MS     500
#define GESTURE_TAP_MAX_MS  300
#define GESTURE_SWIPE_PX    40

static bool g_touch_down = false;
static unsigned long g_touch_down_ms = 0;
static int16_t g_touch_start_x = 0, g_touch_start_y = 0;
static bool g_gesture_fired = false;

static void cycle_screen(void) {
  if (!active_screen) return;
  if      (active_screen == home_screen) goto_screen(nav_screen, "nav (tap)");
  else if (active_screen == nav_screen)  goto_screen(map_screen, "map (tap)");
  else                                 goto_screen(home_screen, "home (tap)");
}

static void handle_gesture_state(void) {
  bool touching = ts_panel.touched();

  if (touching && !g_touch_down) {
    CST_TS_Point p = ts_panel.getPoint(0);
    g_touch_down = true;
    g_gesture_fired = false;
    g_touch_down_ms = millis();
    g_touch_start_x = p.x;
    g_touch_start_y = p.y;
    return;
  }

  if (touching && g_touch_down && !g_gesture_fired) {
    unsigned long held = millis() - g_touch_down_ms;
    CST_TS_Point p = ts_panel.getPoint(0);
    int16_t dx = (int16_t)p.x - g_touch_start_x;
    int16_t dy = (int16_t)p.y - g_touch_start_y;
    int16_t ax = dx < 0 ? -dx : dx;
    int16_t ay = dy < 0 ? -dy : dy;

    if (held >= GESTURE_HOLD_MS) {
      /* long press \u2192 home, from any screen */
      if (active_screen != home_screen) {
        goto_screen(home_screen, "home (hold)");
        toast("Home", C_ACCENT);
      } else {
        toast("Home", C_ACCENT);
      }
      g_gesture_fired = true;
    } else if (held > 120 && ax >= GESTURE_SWIPE_PX && ax > ay) {
      /* horizontal swipe \u2192 brightness (only meaningful on home) */
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
    unsigned long held = millis() - g_touch_down_ms;
    g_touch_down = false;
    if (!g_gesture_fired && held <= GESTURE_TAP_MAX_MS) {
      /* short tap \u2192 cycle screens */
      cycle_screen();
    }
  }
}

/*---------------------------------------------------------------
 * System initialization
 *--------------------------------------------------------------*/

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.printf("[BOOT] ESP32 Arduino %s, LVGL %d.%d.%d, reset_reason=%d\n",
                ESP_ARDUINO_VERSION_STR, LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR,
                LVGL_VERSION_PATCH, (int)esp_reset_reason());

  prefs.begin("lm", false);
  brightness_pct = prefs.getInt("bright", 70);
  prefs.end();

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  pcf8574.pinMode(PCF_TP_RESET,    OUTPUT);   // tp RST
  pcf8574.pinMode(PCF_TP_INT,      OUTPUT);   // tp INT
  pcf8574.pinMode(PCF_LCD_POWER,   OUTPUT);   // lcd power
  pcf8574.pinMode(PCF_LCD_RESET,   OUTPUT);   // lcd reset
  Serial.print("Init pcf8574...");
  Serial.println(pcf8574.begin() ? " OK" : " KO");

  pcf8574.digitalWrite(PCF_LCD_POWER, HIGH);
  delay(100);
  pcf8574.digitalWrite(PCF_LCD_RESET, HIGH);  /* LCD reset sequence */
  delay(100);
  pcf8574.digitalWrite(PCF_LCD_RESET, LOW);
  delay(120);
  pcf8574.digitalWrite(PCF_LCD_RESET, HIGH);
  delay(120);
  pcf8574.digitalWrite(PCF_TP_RESET, HIGH);   /* TP reset sequence */
  delay(100);
  pcf8574.digitalWrite(PCF_TP_RESET, LOW);
  delay(120);
  pcf8574.digitalWrite(PCF_TP_RESET, HIGH);
  delay(120);
  pcf8574.digitalWrite(PCF_TP_INT, HIGH);     /* TP IRQ pull-up */
  delay(120);

  ledcAttach(SCREEN_BACKLIGHT_PIN, PWM_FREQ, PWM_RES);
  apply_brightness(brightness_pct);

  gfx->begin();
  /* Keep MADCTL BGR bit set for this ST7701 panel (0x36 = 0x08). */
  panel_init_bus->beginWrite();
  panel_init_bus->writeCommand(0x36);
  panel_init_bus->write(0x08);
  panel_init_bus->endWrite();
  gfx->fillScreen(0x0000);

  Serial.print("Waiting for touchscreen... ");
  if (!ts_panel.begin(&Wire, I2C_TOUCH_ADDR)) {
    Serial.println("NOT FOUND (continuing without touch \u2014 serial cmds still work)");
  } else {
    Serial.println("found");
  }

  lv_init();
  lv_tick_set_cb(millis);

  size_t buffer_size = sizeof(uint16_t) * screen_width * 48;
  buf1 = (uint8_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM);
  buf2 = (uint8_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM);
  if (!buf1 || !buf2) {
    /* fall back to internal RAM (480*48*2 = 46KB each \u2014 fits) */
    if (!buf1) buf1 = (uint8_t *)malloc(buffer_size);
    if (!buf2) buf2 = (uint8_t *)malloc(buffer_size);
    Serial.printf("[WARN] SPIRAM alloc failed, using internal RAM: buf1=%p buf2=%p\n", buf1, buf2);
  }
  if (!buf1 || !buf2) {
    Serial.println("[FATAL] LVGL framebuffer allocation failed");
    while (true) delay(1000);
  }

  lv_display_t *display = lv_display_create(screen_width, screen_height);
  lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(display, my_disp_flush);
  lv_display_set_buffers(display, buf1, buf2, buffer_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

  lv_indev_t *touch_indev = lv_indev_create();
  lv_indev_set_type(touch_indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(touch_indev, my_touchpad_read);
  lv_indev_set_display(touch_indev, display);

  create_home_screen();
  create_nav_screen();
  create_map_screen();
  create_info_screen();

  /* ---- BLE GATT server (protocol.md §1) ----
   * Server runs in its own FreeRTOS task; callbacks only push into
   * mutex-protected queues that ble_link_poll() drains on the app core. */
  {
    ble_link_cb ble_cb;
    memset(&ble_cb, 0, sizeof ble_cb);
    ble_cb.on_nav      = on_ble_nav;
    ble_cb.on_map_frame = on_ble_map_frame;
    ble_cb.on_ctrl     = on_ble_ctrl;
    ble_link_init(&ble_cb);
    ble_ready = true;
    Serial.println("[BLE] GATT server up: NavData/Status/MapData/Control");
    Serial.printf("[BLE] adv=%s name=%s\n", "on", BLE_NAME);
  }

  ble_timer = lv_timer_create(ble_blink_cb, 500, NULL);

  goto_screen(home_screen, "home");
  Serial.println("Libre-Moto ready. Gestures: tap=cycle, hold=home, swipe=brightness");
  Serial.println("Serial cmds: h n m | b<0-100> | {\"v\":1,\"t\":...} (protocol.md §2)");
}

void loop() {
  handle_serial();
  ble_link_poll();          /* drain NavData/Control/MapData queues, adv state */
  handle_gesture_state();
  handle_state_timeouts();
  lv_timer_handler();
  delay(5);
}
