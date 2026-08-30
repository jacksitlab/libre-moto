#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#include <esp_system.h>
#include <Adafruit_CST8XX.h>
#include <PCF8574.h>
#include <Preferences.h>
#include "config.h"
#include "../nav/nav_message.h"   /* NavData protocol v1 parser (protocol.md §2) */

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
static lv_obj_t *map_route = NULL;
static lv_obj_t *map_road1 = NULL;
static lv_obj_t *map_road2 = NULL;
static lv_obj_t *map_vehicle = NULL;
static lv_obj_t *map_label = NULL;

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

  /* placeholder roads (demo geometry \u2014 replaced by MapFrame renderer next task) */
  map_road1 = lv_line_create(map_screen);
  lv_point_precise_t r1[2] = {{60, 340}, {440, 280}};
  lv_line_set_points(map_road1, r1, 2);
  lv_obj_set_size(map_road1, 380, 120);
  lv_obj_align(map_road1, LV_ALIGN_CENTER, 10, 130);
  lv_obj_set_style_line_width(map_road1, 4, 0);
  lv_obj_set_style_line_color(map_road1, lv_color_hex(0x5C6773), 0);
  lv_obj_remove_flag(map_road1, LV_OBJ_FLAG_CLICKABLE);

  map_road2 = lv_line_create(map_screen);
  lv_point_precise_t r2[2] = {{320, 60}, {330, 420}};
  lv_line_set_points(map_road2, r2, 2);
  lv_obj_set_size(map_road2, 60, 360);
  lv_obj_align(map_road2, LV_ALIGN_CENTER, 10, 0);
  lv_obj_set_style_line_width(map_road2, 4, 0);
  lv_obj_set_style_line_color(map_road2, lv_color_hex(0x5C6773), 0);
  lv_obj_remove_flag(map_road2, LV_OBJ_FLAG_CLICKABLE);

  map_route = lv_line_create(map_screen);
  lv_point_precise_t rt[5] = {{80, 380}, {200, 300}, {280, 200}, {330, 120}, {345, 70}};
  lv_line_set_points(map_route, rt, 5);
  lv_obj_set_size(map_route, 290, 340);
  lv_obj_align(map_route, LV_ALIGN_CENTER, 0, 10);
  lv_obj_set_style_line_width(map_route, 7, 0);
  lv_obj_set_style_line_color(map_route, C_TEXT, 0);
  lv_obj_remove_flag(map_route, LV_OBJ_FLAG_CLICKABLE);

  /* vehicle: small triangle at center, pointing up (closed by repeating first point) */
  map_vehicle = lv_line_create(map_screen);
  lv_point_precise_t v[4] = {{240, 205}, {222, 250}, {258, 250}, {240, 205}};
  lv_line_set_points(map_vehicle, v, 4);
  lv_obj_set_size(map_vehicle, 50, 60);
  lv_obj_align(map_vehicle, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_line_width(map_vehicle, 5, 0);
  lv_obj_set_style_line_color(map_vehicle, C_ACCENT, 0);
  lv_obj_set_style_bg_color(map_vehicle, C_ACCENT, 0);
  lv_obj_set_style_bg_opa(map_vehicle, LV_OPA_COVER, 0);
  lv_obj_remove_flag(map_vehicle, LV_OBJ_FLAG_CLICKABLE);

  map_label = lv_label_create(map_screen);
  lv_label_set_text(map_label, "map (demo geometry)");
  lv_obj_set_style_text_font(map_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(map_label, C_TEXT2, 0);
  lv_obj_align(map_label, LV_ALIGN_CENTER, 0, -210);
  lv_obj_remove_flag(map_label, LV_OBJ_FLAG_CLICKABLE);
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
      /* heartbeat: keeps the link alive; does not change state (protocol.md) */
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
 * BLE status dot: color follows the connection/app state
 *   IDLE   → amber, 500 ms blink ("standby")
 *   NAV    → green, steady
 *   LOST   → red, steady          (*dot not visible on those screens,
 *   ARRIVED→ green, steady          but the timer keeps the right state)
 *--------------------------------------------------------------*/

static void ble_blink_cb(lv_timer_t *t) {
  (void)t;
  static bool on = true;
  if (app == APP_IDLE) {
    on = !on;
    if (ble_dot) lv_obj_set_style_bg_color(ble_dot, on ? C_WARN : lv_color_hex(0x3B4148), 0);
    if (ble_label) lv_label_set_text(ble_label, "BLE: standby");
  } else {
    static const lv_color_t c[4] = { C_WARN, C_ACCENT, C_ACCENT, C_BAD };
    (void)c;
    if (ble_dot) lv_obj_set_style_bg_color(ble_dot, (app == APP_LOST) ? C_BAD : C_ACCENT, 0);
    if (ble_label) {
      if (app == APP_LOST)       lv_label_set_text(ble_label, "BLE: link lost");
      else if (app == APP_ARRIVED) lv_label_set_text(ble_label, "BLE: connected \u2713");
      else                        lv_label_set_text(ble_label, "BLE: connected \u2713");
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

/*---------------------------------------------------------------
 * Serial debug channel (until BLE + phone app are in)
 *   h / n / m              → home / nav / map (quick jump)
 *   b<0-100>               → brightness
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

  ble_timer = lv_timer_create(ble_blink_cb, 500, NULL);

  goto_screen(home_screen, "home");
  Serial.println("Libre-Moto ready. Gestures: tap=cycle, hold=home, swipe=brightness");
  Serial.println("Serial cmds: h n m | b<0-100> | {\"v\":1,\"t\":...} (protocol.md §2)");
}

void loop() {
  handle_serial();
  handle_gesture_state();
  handle_state_timeouts();
  lv_timer_handler();
  delay(5);
}
