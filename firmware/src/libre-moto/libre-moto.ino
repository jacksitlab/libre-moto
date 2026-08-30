#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#include <esp_system.h>
#include <Adafruit_CST8XX.h>
#include <PCF8574.h>
#include <Preferences.h>
#include "config.h"

/*---------------------------------------------------------------
 * Hardware (CrowPanel 2.1" — see docs/hardware.md)
 *--------------------------------------------------------------*/

PCF8574 pcf8574(PCF8574_ADDR);                 // I/O expander (reset/IRQ pins)
Adafruit_CST8XX tsPanel = Adafruit_CST8XX();   // CST816 capacitive touch

static const uint16_t screenWidth  = SCREEN_W;
static const uint16_t screenHeight = SCREEN_H;

/* ---- brightness (persisted) ---- */
Preferences prefs;
static int brightnessPct = 70;

/* ---- LVGL objects (screens + shared) ---- */
static uint8_t *buf1 = NULL;
static uint8_t *buf2 = NULL;

static lv_obj_t *homeScreen = NULL;
static lv_obj_t *homeTitle = NULL;
static lv_obj_t *bleDot = NULL;
static lv_obj_t *bleLabel = NULL;
static lv_obj_t *homeMsg = NULL;
static lv_obj_t *brightBar = NULL;
static lv_obj_t *toastLabel = NULL;
static lv_timer_t *toastTimer = NULL;

static lv_obj_t *navScreen = NULL;
static lv_obj_t *navArrow = NULL;   // polyline, big maneuver arrow
static lv_obj_t *navDist = NULL;    // "350 m"
static lv_obj_t *navText = NULL;    // road name
static lv_obj_t *navStatus = NULL;  // "Link lost" / "Arrived" / ""

static lv_obj_t *mapScreen = NULL;
static lv_obj_t *mapRoute = NULL;
static lv_obj_t *mapRoad1 = NULL;
static lv_obj_t *mapRoad2 = NULL;
static lv_obj_t *mapVehicle = NULL;
static lv_obj_t *mapLabel = NULL;

static lv_obj_t *activeScreen = NULL;
static lv_timer_t *bleTimer = NULL;
static bool bleDotOn = false;

/* colors (dark theme, see docs/design.md) */
static const lv_color_t C_BG      = lv_color_hex(0x0D1117);
static const lv_color_t C_TEXT    = lv_color_hex(0xE6EDF3);
static const lv_color_t C_TEXT2   = lv_color_hex(0x8B949E);
static const lv_color_t C_ACCENT  = lv_color_hex(0x3FB950);
static const lv_color_t C_WARN    = lv_color_hex(0xD29922);

/*---------------------------------------------------------------
 * RGB display driver configuration (ST7701S, type5 init)
 *--------------------------------------------------------------*/

Arduino_DataBus *panelInitBus = new Arduino_SWSPI(
  GFX_NOT_DEFINED /* DC: ST7701 uses 9-bit SPI */, PIN_CS,
  PIN_SCK, PIN_SDA, GFX_NOT_DEFINED /* MISO */);

Arduino_ESP32RGBPanel *rgbPanel = new Arduino_ESP32RGBPanel(
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
  SCREEN_W, SCREEN_H, rgbPanel, 0 /* rotation */, true /* auto flush */,
  panelInitBus, GFX_NOT_DEFINED /* RST (via PCF8574 P4) */,
  st7701_type5_init_operations, sizeof(st7701_type5_init_operations));

/*---------------------------------------------------------------
 * LVGL display output
 *--------------------------------------------------------------*/

void my_disp_flush(lv_display_t *display, const lv_area_t *area, uint8_t *px_map) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
  /* This ST7701 wiring needs the R/B 5-bit fields swapped. */
  uint16_t *pixels = (uint16_t *)px_map;
  const uint32_t pixelCount = w * h;
  for (uint32_t i = 0; i < pixelCount; ++i) {
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
  if (tsPanel.touched()) {
    CST_TS_Point p = tsPanel.getPoint(0);
    /* Keep the last point for tiny moves so LVGL doesn't redraw a pressed
       object on every noisy sample. */
    static int16_t stableX = -1;
    static int16_t stableY = -1;
    const int16_t touchX = constrain(p.x, 0, screenWidth - 1);
    const int16_t touchY = constrain(p.y - TOUCH_Y_OFFSET, 0, screenHeight - 1);
    if (stableX < 0 || abs(touchX - stableX) >= 4 || abs(touchY - stableY) >= 4) {
      stableX = touchX;
      stableY = touchY;
    }
    data->point.x = stableX;
    data->point.y = stableY;
    data->state = LV_INDEV_STATE_PR;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

/*---------------------------------------------------------------
 * Backlight
 *--------------------------------------------------------------*/

static void applyBrightness(int pct) {
  brightnessPct = constrain(pct, BRIGHTNESS_MIN_PCT, 100);
  ledcWrite(SCREEN_BACKLIGHT_PIN, (brightnessPct * 255) / 100);
  if (brightBar) lv_bar_set_value(brightBar, brightnessPct, LV_ANIM_OFF);
  /* Persist async-ish: NVS writes are cheap at 2 Hz */
  prefs.begin("lm", false);
  prefs.putInt("bright", brightnessPct);
  prefs.end();
}

/*---------------------------------------------------------------
 * UI — screen builders
 *--------------------------------------------------------------*/

static lv_obj_t *newScreen() {
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_remove_style_all(scr);
  lv_obj_set_size(scr, SCREEN_W, SCREEN_H);
  lv_obj_set_style_bg_color(scr, C_BG, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  return scr;
}

static void createHomeScreen(void) {
  homeScreen = newScreen();

  homeTitle = lv_label_create(homeScreen);
  lv_label_set_text(homeTitle, "LibreMoto");
  lv_obj_set_style_text_font(homeTitle, &lv_font_montserrat_34, 0);
  lv_obj_set_style_text_color(homeTitle, C_TEXT, 0);
  lv_obj_align(homeTitle, LV_ALIGN_CENTER, 0, -120);

  bleDot = lv_obj_create(homeScreen);
  lv_obj_remove_style_all(bleDot);
  lv_obj_set_size(bleDot, 18, 18);
  lv_obj_set_style_radius(bleDot, 9, 0);
  lv_obj_set_style_bg_color(bleDot, C_WARN, 0);
  lv_obj_set_style_bg_opa(bleDot, LV_OPA_COVER, 0);
  lv_obj_align(bleDot, LV_ALIGN_CENTER, -90, -42);

  bleLabel = lv_label_create(homeScreen);
  lv_label_set_text(bleLabel, "BLE: standby");
  lv_obj_set_style_text_font(bleLabel, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(bleLabel, C_TEXT2, 0);
  lv_obj_align(bleLabel, LV_ALIGN_CENTER, 30, -42);

  homeMsg = lv_label_create(homeScreen);
  lv_label_set_text(homeMsg, "Ready.\nTap: next screen  \u2022  Hold: home\nSwipe left/right: brightness");
  lv_obj_set_style_text_font(homeMsg, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(homeMsg, C_TEXT2, 0);
  lv_obj_align(homeMsg, LV_ALIGN_CENTER, 0, 30);

  brightBar = lv_bar_create(homeScreen);
  lv_obj_set_size(brightBar, 240, 14);
  lv_obj_set_style_bg_color(brightBar, C_TEXT2, 0);
  lv_obj_set_style_bg_opa(brightBar, LV_OPA_30, 0);
  lv_obj_set_style_radius(brightBar, 7, 0);
  lv_obj_set_style_bg_color(brightBar, C_ACCENT, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(brightBar, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_align(brightBar, LV_ALIGN_CENTER, 0, 110);
  lv_obj_remove_flag(brightBar, LV_OBJ_FLAG_CLICKABLE);
  lv_bar_set_range(brightBar, 0, 100);
  lv_bar_set_value(brightBar, brightnessPct, LV_ANIM_OFF);
}

/* Maneuver arrow (lv_line, 120\u00d7120 box at object pos) */
static void setArrow(const char *dir) {
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
  lv_line_set_points(navArrow, pts, (uint32_t)n);
}

static void createNavScreen(void) {
  navScreen = newScreen();

  navArrow = lv_line_create(navScreen);
  lv_obj_set_size(navArrow, 120, 120);
  lv_obj_align(navArrow, LV_ALIGN_CENTER, -170, 0);
  lv_obj_set_style_line_width(navArrow, 12, 0);
  lv_obj_set_style_line_color(navArrow, C_TEXT, 0);
  lv_obj_set_style_line_rounded(navArrow, true, 0);
  lv_obj_remove_flag(navArrow, LV_OBJ_FLAG_CLICKABLE);

  navDist = lv_label_create(navScreen);
  lv_label_set_text(navDist, "350 m");
  lv_obj_set_style_text_font(navDist, &lv_font_montserrat_34, 0);
  lv_obj_set_style_text_color(navDist, C_TEXT, 0);
  lv_obj_align(navDist, LV_ALIGN_CENTER, 40, -20);
  lv_obj_remove_flag(navDist, LV_OBJ_FLAG_CLICKABLE);

  navText = lv_label_create(navScreen);
  lv_label_set_text(navText, "Main St.");
  lv_obj_set_style_text_font(navText, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(navText, C_TEXT2, 0);
  lv_obj_align(navText, LV_ALIGN_CENTER, 40, 22);
  lv_obj_remove_flag(navText, LV_OBJ_FLAG_CLICKABLE);

  navStatus = lv_label_create(navScreen);
  lv_label_set_text(navStatus, "");
  lv_obj_set_style_text_font(navStatus, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(navStatus, C_WARN, 0);
  lv_obj_align(navStatus, LV_ALIGN_CENTER, 0, 150);

  setArrow("left");
}

static void createMapScreen(void) {
  mapScreen = newScreen();

  /* placeholder roads (demo geometry \u2014 replaced by MapFrame renderer next task) */
  mapRoad1 = lv_line_create(mapScreen);
  lv_point_precise_t r1[2] = {{60, 340}, {440, 280}};
  lv_line_set_points(mapRoad1, r1, 2);
  lv_obj_set_size(mapRoad1, 380, 120);
  lv_obj_align(mapRoad1, LV_ALIGN_CENTER, 10, 130);
  lv_obj_set_style_line_width(mapRoad1, 4, 0);
  lv_obj_set_style_line_color(mapRoad1, lv_color_hex(0x5C6773), 0);
  lv_obj_remove_flag(mapRoad1, LV_OBJ_FLAG_CLICKABLE);

  mapRoad2 = lv_line_create(mapScreen);
  lv_point_precise_t r2[2] = {{320, 60}, {330, 420}};
  lv_line_set_points(mapRoad2, r2, 2);
  lv_obj_set_size(mapRoad2, 60, 360);
  lv_obj_align(mapRoad2, LV_ALIGN_CENTER, 10, 0);
  lv_obj_set_style_line_width(mapRoad2, 4, 0);
  lv_obj_set_style_line_color(mapRoad2, lv_color_hex(0x5C6773), 0);
  lv_obj_remove_flag(mapRoad2, LV_OBJ_FLAG_CLICKABLE);

  mapRoute = lv_line_create(mapScreen);
  lv_point_precise_t rt[5] = {{80, 380}, {200, 300}, {280, 200}, {330, 120}, {345, 70}};
  lv_line_set_points(mapRoute, rt, 5);
  lv_obj_set_size(mapRoute, 290, 340);
  lv_obj_align(mapRoute, LV_ALIGN_CENTER, 0, 10);
  lv_obj_set_style_line_width(mapRoute, 7, 0);
  lv_obj_set_style_line_color(mapRoute, C_TEXT, 0);
  lv_obj_remove_flag(mapRoute, LV_OBJ_FLAG_CLICKABLE);

  /* vehicle: small triangle at center, pointing up (closed by repeating first point) */
  mapVehicle = lv_line_create(mapScreen);
  lv_point_precise_t v[4] = {{240, 205}, {222, 250}, {258, 250}, {240, 205}};
  lv_line_set_points(mapVehicle, v, 4);
  lv_obj_set_size(mapVehicle, 50, 60);
  lv_obj_align(mapVehicle, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_line_width(mapVehicle, 5, 0);
  lv_obj_set_style_line_color(mapVehicle, C_ACCENT, 0);
  lv_obj_set_style_bg_color(mapVehicle, C_ACCENT, 0);
  lv_obj_set_style_bg_opa(mapVehicle, LV_OPA_COVER, 0);
  lv_obj_remove_flag(mapVehicle, LV_OBJ_FLAG_CLICKABLE);

  mapLabel = lv_label_create(mapScreen);
  lv_label_set_text(mapLabel, "map (demo geometry)");
  lv_obj_set_style_text_font(mapLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(mapLabel, C_TEXT2, 0);
  lv_obj_align(mapLabel, LV_ALIGN_CENTER, 0, -210);
  lv_obj_remove_flag(mapLabel, LV_OBJ_FLAG_CLICKABLE);
}

/*---------------------------------------------------------------
 * BLE status dot animation (500 ms blink, standby color)
 *--------------------------------------------------------------*/

static void bleBlinkCb(lv_timer_t *t) {
  (void)t;
  bleDotOn = !bleDotOn;
  if (bleDot) lv_obj_set_style_bg_color(bleDot, bleDotOn ? C_WARN : lv_color_hex(0x3B4148), 0);
  if (bleLabel) {
    if (bleDotOn) lv_label_set_text(bleLabel, "BLE: standby");
  }
}

/*---------------------------------------------------------------
 * Toast (short feedback overlay, auto-hidden)
 *--------------------------------------------------------------*/

static void toastHideCb(lv_timer_t *t) {
  (void)t;
  if (toastLabel) lv_obj_del(toastLabel);
  toastLabel = NULL;
  lv_timer_del(toastTimer);
  toastTimer = NULL;
}

static void toast(const char *text, lv_color_t color) {
  if (toastLabel) lv_obj_del(toastLabel);
  toastLabel = NULL;
  toastLabel = lv_label_create(lv_screen_active());
  lv_obj_add_flag(toastLabel, LV_OBJ_FLAG_FLOATING);
  lv_obj_set_style_bg_color(toastLabel, lv_color_hex(0x161B22), 0);
  lv_obj_set_style_bg_opa(toastLabel, LV_OPA_90, 0);
  lv_obj_set_style_pad_hor(toastLabel, 16, 0);
  lv_obj_set_style_pad_ver(toastLabel, 10, 0);
  lv_obj_set_style_radius(toastLabel, 10, 0);
  lv_obj_set_style_text_font(toastLabel, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(toastLabel, color, 0);
  lv_label_set_text(toastLabel, text);
  lv_obj_center(toastLabel);
  if (toastTimer) lv_timer_del(toastTimer);
  toastTimer = lv_timer_create(toastHideCb, 800, NULL);
}

/*---------------------------------------------------------------
 * Serial debug channel (until BLE + phone app are in)
 *   h / n / m                \u2192 home / nav / map
 *   b<0-100>                \u2192 brightness
 *   nav <dir> <m> <text>    \u2192 set nav instruction + go to nav screen
 *--------------------------------------------------------------*/

static void gotoScreen(lv_obj_t *scr, const char *name) {
  activeScreen = scr;
  lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
  Serial.printf("[UI] -> %s\n", name);
}

static void setNav(const char *dir, int dist_m, const char *text) {
  setArrow(dir);
  char d[16];
  snprintf(d, sizeof(d), "%d m", dist_m);
  lv_label_set_text(navDist, d);
  lv_label_set_text(navText, text && text[0] ? text : "");
  Serial.printf("[UI] nav: %s | %d m | %s\n", dir, dist_m, text ? text : "");
}

static void handleSerial(void) {
  static char line[96];
  static int li = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (li == 0) continue;
      line[li] = 0;
      if (*line == 'h')                gotoScreen(homeScreen, "home");
      else if (*line == 'n')           gotoScreen(navScreen, "nav");
      else if (*line == 'm')           gotoScreen(mapScreen, "map");
      else if (*line == 'b') {
        applyBrightness(atoi(line + 1));
        Serial.printf("[UI] brightness %d%%\n", brightnessPct);
      }
      else if (!strncmp(line, "nav ", 4)) {
        char dir[16] = "left", text[32] = "";
        int dist = 999;
        char *sp = strtok(line + 4, " \t");
        if (sp) strncpy(dir, sp, 15);
        sp = strtok(NULL, " \t");
        if (sp) dist = atoi(sp);
        sp = strtok(NULL, " \t");
        if (sp) strncpy(text, sp, 31);
        setNav(dir, dist, text);
        gotoScreen(navScreen, "nav");
      }
      li = 0;
    } else if (li < (int)sizeof(line) - 1) {
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

static bool gTouchDown = false;
static unsigned long gTouchDownMs = 0;
static int16_t gTouchStartX = 0, gTouchStartY = 0;
static bool gGestureFired = false;

static void cycleScreen(void) {
  if (!activeScreen) return;
  if      (activeScreen == homeScreen) gotoScreen(navScreen, "nav (tap)");
  else if (activeScreen == navScreen)  gotoScreen(mapScreen, "map (tap)");
  else                                 gotoScreen(homeScreen, "home (tap)");
}

static void handleGestureState(void) {
  bool touching = tsPanel.touched();

  if (touching && !gTouchDown) {
    CST_TS_Point p = tsPanel.getPoint(0);
    gTouchDown = true;
    gGestureFired = false;
    gTouchDownMs = millis();
    gTouchStartX = p.x;
    gTouchStartY = p.y;
    return;
  }

  if (touching && gTouchDown && !gGestureFired) {
    unsigned long held = millis() - gTouchDownMs;
    CST_TS_Point p = tsPanel.getPoint(0);
    int16_t dx = (int16_t)p.x - gTouchStartX;
    int16_t dy = (int16_t)p.y - gTouchStartY;
    int16_t ax = dx < 0 ? -dx : dx;
    int16_t ay = dy < 0 ? -dy : dy;

    if (held >= GESTURE_HOLD_MS) {
      /* long press \u2192 home, from any screen */
      if (activeScreen != homeScreen) {
        gotoScreen(homeScreen, "home (hold)");
        toast("Home", C_ACCENT);
      } else {
        toast("Home", C_ACCENT);
      }
      gGestureFired = true;
    } else if (held > 120 && ax >= GESTURE_SWIPE_PX && ax > ay) {
      /* horizontal swipe \u2192 brightness (only meaningful on home) */
      if (!activeScreen || activeScreen == homeScreen) {
        int delta = dx > 0 ? 10 : -10;
        applyBrightness(brightnessPct + delta);
        char buf[40];
        snprintf(buf, sizeof(buf), "Brightness %d%%", brightnessPct);
        toast(buf, C_TEXT);
      }
      gGestureFired = true;
    }
    return;
  }

  if (!touching && gTouchDown) {
    unsigned long held = millis() - gTouchDownMs;
    gTouchDown = false;
    if (!gGestureFired && held <= GESTURE_TAP_MAX_MS) {
      /* short tap \u2192 cycle screens */
      cycleScreen();
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
  brightnessPct = prefs.getInt("bright", 70);
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
  applyBrightness(brightnessPct);

  gfx->begin();
  /* Keep MADCTL BGR bit set for this ST7701 panel (0x36 = 0x08). */
  panelInitBus->beginWrite();
  panelInitBus->writeCommand(0x36);
  panelInitBus->write(0x08);
  panelInitBus->endWrite();
  gfx->fillScreen(0x0000);

  Serial.print("Waiting for touchscreen... ");
  if (!tsPanel.begin(&Wire, I2C_TOUCH_ADDR)) {
    Serial.println("NOT FOUND (continuing without touch \u2014 serial cmds still work)");
  } else {
    Serial.println("found");
  }

  lv_init();
  lv_tick_set_cb(millis);

  size_t buffer_size = sizeof(uint16_t) * screenWidth * 48;
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

  lv_display_t *display = lv_display_create(screenWidth, screenHeight);
  lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(display, my_disp_flush);
  lv_display_set_buffers(display, buf1, buf2, buffer_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

  lv_indev_t *touch_indev = lv_indev_create();
  lv_indev_set_type(touch_indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(touch_indev, my_touchpad_read);
  lv_indev_set_display(touch_indev, display);

  createHomeScreen();
  createNavScreen();
  createMapScreen();

  bleTimer = lv_timer_create(bleBlinkCb, 500, NULL);

  gotoScreen(homeScreen, "home");
  Serial.println("Libre-Moto ready. Gestures: tap=cycle, hold=home, swipe=brightness");
  Serial.println("Serial cmds: h n m | b<0-100> | nav <dir> <m> <text>");
}

void loop() {
  handleSerial();
  handleGestureState();
  lv_timer_handler();
  delay(5);
}
