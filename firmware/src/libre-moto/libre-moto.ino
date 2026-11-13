#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#include <esp_system.h>
#include <Adafruit_CST8XX.h>
#include <PCF8574.h>
#include <Preferences.h>
#include <math.h>
#include <FreeRTOS.h>
#include "config.h"
#include "../nav/nav_message.h" /* NavData + Control protocol v1 (protocol.md §2, §5) */
#include "../ble/ble_link.h"   /* GATT server: NavData/Status/MapData/Control         */
#include "../map/map_frame.h"  /* MapData binary frames (protocol.md §4)              */
#include "../map/map_render.h" /* prepare_map_frame(): geometry stage (comm thread)   */
#include "../map/map_blit.h"   /* full-frame software renderer (replaces the line pool) */
#include "libremoto_icon.h"    /* the LibreMoto mark (generated from docs/icon.svg)    */
#include "libremoto_logo.h"    /* the wordmark (generated from docs/logo.svg)         */
#include "maneuver_icons.h"   /* maneuver arrows (generated from docs/assets/maneuvers) */
#include "vehicle_icon.h"     /* vehicle marker (generated from docs/assets/vehicle.svg) */

/* LVGL 9 image descriptor for the icon (ARGB8888, 200×200). */
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
};

/* LVGL 9 image descriptor for the wordmark — I8 INDEXED (256-entry
 * ARGB8888 palette inside the data, then 1 byte per pixel). The logo
 * is 3 colors + antialiasing edges; indexed storage cuts 360 KB down
 * to 91 KB of flash. The bin decoder expands palette lines on draw. */
static const lv_image_dsc_t libremoto_logo = {
  .header = {
    .magic = LV_IMAGE_HEADER_MAGIC,
    .cf = LV_COLOR_FORMAT_I8,
    .flags = 0,
    .w = 480,
    .h = 188,
    .stride = 480,          /* 1 byte per pixel */
    .reserved_2 = 0,
  },
  .data_size = 256 * 4 + 480 * 188,
  .data = libremoto_logo_map,
};

/* Maneuver arrow icons (48×48 ARGB8888, generated from
 * docs/assets/maneuvers/*.svg — replace the SVGs and re-run
 * scripts/svg2maneuvers.py to restyle them). */
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
  };
MANEUVER_ICON_DSC(left)
MANEUVER_ICON_DSC(right)
MANEUVER_ICON_DSC(straight)
MANEUVER_ICON_DSC(uturn)
MANEUVER_ICON_DSC(merge)
MANEUVER_ICON_DSC(slight_left)
MANEUVER_ICON_DSC(slight_right)

/* Vehicle marker icon (64×64 ARGB8888, generated from
 * docs/assets/vehicle.svg — replace the SVG and re-run the script). */
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
};

/** Wire keyword → icon descriptor (NULL = unknown → straight). */
/** Wire keyword → raw ARGB8888 map (48×48) for map_blit_vehicle. */
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
static lv_obj_t *home_icon = NULL;
static lv_obj_t *ble_dot = NULL;
static lv_obj_t *toast_label = NULL;
static lv_timer_t *toast_timer = NULL;


static lv_obj_t *map_screen = NULL;
/* segment pool: MAP_POOL lines, created once, refilled per frame.
   44 = 20 roads × (casing+fill) + route + destination + margin — the
   ESP32 line budget for the two-pass casing+fill render. */
#define MAP_POOL      44
static uint16_t *g_map_fb = NULL;       /* 480×480 RGB565, PSRAM — the map's own framebuffer */
#define MAP_CX 240   /* vehicle marker x (one third from the bottom) */
#define MAP_CY 320   /* vehicle marker y */
static lv_obj_t *map_img = NULL;        /* the map as ONE image object (cheap redraws) */
static lv_image_dsc_t g_map_fb_dsc;     /* descriptor over g_map_fb (RGB565) */
static lv_obj_t *map_vehicle = NULL;        /* vehicle marker icon (SVG) */
static lv_obj_t *map_marker = NULL;    /* destination marker (rotating frame) */
static lv_obj_t *map_scale = NULL;     /* scale bar line */
static lv_obj_t *map_label = NULL;     /* heading / scale label */
static lv_obj_t *map_nav_panel = NULL; /* bottom info panel (Beeline style) */
static lv_obj_t *map_nav_arrow = NULL; /* maneuver arrow, inside the panel */
static lv_obj_t *map_nav_dist  = NULL;  /* "350 m", inside the panel */
static lv_obj_t *map_limit = NULL;     /* speed limit sign circle */
static lv_obj_t *map_limit_label = NULL; /* ... the number inside */

static lv_obj_t *active_screen = NULL;
static lv_timer_t *ble_timer = NULL;
static bool ble_dot_on = false;

/* ---- navigation state (from NavData, protocol.md §2) ---- */
static NavState    g_nav;            /* last valid parsed state            */
static bool        have_nav = false; /* at least one valid frame received  */
static uint32_t    rx_count = 0;     /* received frames (BLE-ready count)  */
static uint32_t    rx_ok_count = 0;   /* ... of which parsed OK             */
static unsigned long last_data_ms = 0;/* millis() of last valid frame       */
static unsigned long g_splash_until_ms = 0; /* 0 = no splash pending        */
static lv_obj_t *g_splash_screen = NULL;    /* boot splash (deleted on exit) */
static AppState    app = APP_IDLE;

/* ---- MapFrame statistics (protocol.md §3/§4; render = C.1) ---- */
static uint8_t  map_seq_last = 0;     /* last OK frame seq     */
static uint32_t map_drop_count = 0;   /* dropped/stale/incomplete frames */
static bool     map_seq_seen = false; /* any OK frame yet?     */
static bool     ble_ready = false;    /* BLE link module up    */
static unsigned long state_until_ms = 0;

/*---------------------------------------------------------------
 * Threading hand-off (comm task → display task)
 *
 * Two FreeRTOS tasks split the work so a big MapData frame never
 * blocks the screen:
 *
 *   COMM task (comm_task): ble_link_poll() → parse → geometry
 *     (prepare_map_frame). Publishes:
 *       - g_map_out  : latest prepared MapRenderFrame (mutex)
 *       - ui_q       : parsed NavState / CtrlState (mutex)
 *     NO LVGL calls on this task.
 *
 *   DISPLAY task (Arduino loop): drains ui_q, blits the prepared
 *     map frame into the LVGL pool, handles touch/gestures, state
 *     timeouts and lv_timer_handler(). ALL LVGL calls live here.
 *
 * Serial stays on the display task (debug channel, calls UI
 * directly).
 *--------------------------------------------------------------*/
static xSemaphoreHandle g_map_mux = NULL;   /* guards g_map_out + g_map_ready */
static MapRenderFrame g_map_out;            /* ~29 KB — lives in .bss */
static volatile bool   g_map_ready = false; /* a prepared frame is waiting */

static xSemaphoreHandle g_ui_mux = NULL;    /* guards the ui_q ring */
static xSemaphoreHandle g_map_parse_mux = NULL; /* serializes on_ble_map_frame
                                                  (BLE comm task + serial mock
                                                  share the static MapFrame) */
#define UI_Q_SIZE 16
struct ui_msg {
  int      kind;   /* UI_MSG_NAV / UI_MSG_CTRL */
  NavState nav;    /* valid when kind == UI_MSG_NAV */
  CtrlState ctrl;  /* valid when kind == UI_MSG_CTRL */
};
static struct ui_msg ui_q[UI_Q_SIZE];
static uint16_t ui_head = 0, ui_tail = 0, ui_free = 0;

enum { UI_MSG_NAV = 1, UI_MSG_CTRL };

/* Comm task → display task: enqueue a parsed message. Drops when the
   ring is full (the display task drains every loop iteration). */
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

extern uint32_t diag_flush_ms;
/* Scratch line for the R/B swap: in DIRECT mode px_map IS the display
 * framebuffer (our PSRAM map fb) — swapping in place would corrupt the
 * map for every subsequent LVGL read (white flicker). Swap into a
 * scratch line buffer instead, one row at a time. 480 px = 960 B. */
static uint16_t s_flush_scratch[SCREEN_W];

void my_disp_flush(lv_display_t *display, const lv_area_t *area, uint8_t *px_map) {
  const uint32_t w = (area->x2 - area->x1 + 1);
  const uint32_t h = (area->y2 - area->y1 + 1);
  /* DIRECT-MODE STRIDE (the invisible-dot bug): LVGL hands us the
     FULL-SCREEN draw buffer — the flushed area is a WINDOW inside it,
     so the row stride is the SCREEN width (verified in lv_refr.c:
     call_flush_cb passes layer->draw_buf->data unchanged). The old
     code indexed rows with the AREA width as stride, reading
     diagonally through the buffer for every partial flush: the BLE
     dot's 22x22 region got background pixels painted over it —
     visible once at screen load (full-area flush, stride matched by
     accident) then 'erased' by the first blink tick. Same for every
     other small invalidation (toasts, arrows). */
  /* px_map IS the full-screen buffer (verified: pointer == our fb) and
     the flushed area is a WINDOW in it. Absolute indexing:
     src[(area_y1 + row) * scr_w + area_x1 + col]. The first fix attempt
     used row*scr_w (area-relative rows, absolute stride) — still wrong;
     the headless repro caught it: the dot showed the OLD colour after
     every flush. Repro-verified correct indexing below. */
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
    gfx->draw16bitRGBBitmap(area->x1, area->y1 + (int16_t)row, s_flush_scratch, w, 1);
  }
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

  /* The LibreMoto mark (motorcycle icon from docs/icon.svg), centered,
   * large — the minimal home screen: just the icon + BLE status dot. */
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
  /* DIRECT mode (verified headless): the screen bg must be OPAQUE
     BLACK. A transparent bg makes LVGL fill the invalidated regions
     WHITE (white screen!). With opaque black the semantics are:
     - screen switch → one full refresh fills black (fine)
     - map_blit paints the map into the fb AFTER that
     - overlay changes invalidate ONLY their own regions; LVGL fills
       those black and draws the (opaque) overlay on top — the map
       pixels everywhere else are never touched. */
  lv_obj_set_style_bg_color(map_screen, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(map_screen, LV_OPA_COVER, 0);

  /* ---- the map itself: ONE image object over a PSRAM framebuffer ----
   * Replaces the 44-object lv_line pool: the SW renderer needed ~150 ms
   * per full redraw; map_blit paints the same geometry directly into
   * g_map_fb (~10-20 ms) and LVGL only blits one image. Created FIRST
   * so all overlays (vehicle, marker, band, sign) draw on top. */
  if (g_map_fb) {
    memset(&g_map_fb_dsc, 0, sizeof g_map_fb_dsc);
    g_map_fb_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    g_map_fb_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    g_map_fb_dsc.header.w = SCREEN_W;
    g_map_fb_dsc.header.h = SCREEN_H;
    g_map_fb_dsc.header.stride = SCREEN_W * 2;
    g_map_fb_dsc.data_size = (uint32_t)SCREEN_W * SCREEN_H * 2;
    g_map_fb_dsc.data = (const uint8_t *)g_map_fb;
    /* clear once so the first frame isn't garbage */
    for (size_t i = 0; i < (size_t)SCREEN_W * SCREEN_H; i++) g_map_fb[i] = 0;
    /* DIRECT mode: no image object — the fb is the display buffer and
       map_blit paints straight into it. */
    Serial.println("[MAP] full-frame renderer: DIRECT fb mode");
  } else {
    /* PARTIAL fallback: a 480×480 RGB565 fb needs 450 KB — ONLY PSRAM
       can hold it. Without PSRAM (board option 'PSRAM: op' not set in
       the Arduino IDE!) there is NO map: the full-frame renderer needs
       the fb. Fail LOUDLY at boot instead of a silent black map. */
    Serial.println("[MAP] FATAL: no PSRAM — enable 'PSRAM: OPI PSRAM' in the");
    Serial.println("[MAP]        board options (Tools menu) and re-flash!");
  }

  /* ---- vehicle marker: SVG-rendered icon (docs/assets/vehicle.svg),
     centred on the vehicle position (MAP_CX/MAP_CY = 240/320). The
     icon's tip points UP — the firmware rotates the world, not the
     marker. */
  map_vehicle = lv_img_create(map_screen);
  lv_img_set_src(map_vehicle, &vehicle_icon);
  lv_img_set_antialias(map_vehicle, true);
  lv_obj_set_pos(map_vehicle, 240 - 32, 320 - 32);
  lv_obj_remove_flag(map_vehicle, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_CHAIN));

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
  lv_label_set_text(map_label, "Waiting for map data…");
  lv_obj_set_style_text_font(map_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(map_label, C_TEXT2, 0);
  lv_obj_align(map_label, LV_ALIGN_CENTER, 0, -215);
  lv_obj_remove_flag(map_label, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_CHAIN));
  lv_obj_add_flag(map_label, LV_OBJ_FLAG_HIDDEN);

  /* ---- nav info panel (bottom, Beeline style): maneuver arrow +
     distance on a semi-transparent dark panel below the vehicle marker,
     visually separated from the map. The map is THE navigation view. */
  map_nav_panel = lv_obj_create(map_screen);
  lv_obj_remove_style_all(map_nav_panel);
  /* full-width band, flush to the bottom edge (user request: from the
     far left to the far right, all the way down) */
  lv_obj_set_size(map_nav_panel, 480, 84);
  lv_obj_align(map_nav_panel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_set_style_radius(map_nav_panel, 0, 0);
  lv_obj_set_style_bg_color(map_nav_panel, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(map_nav_panel, LV_OPA_60, 0);   /* semi-transparent (Beeline style) */
  lv_obj_set_style_border_width(map_nav_panel, 0, 0);
  lv_obj_remove_flag(map_nav_panel, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_CHAIN));

  map_nav_arrow = lv_img_create(map_screen);
  lv_img_set_src(map_nav_arrow, &maneuver_icon_straight);
  lv_img_set_antialias(map_nav_arrow, true);
  lv_obj_align(map_nav_arrow, LV_ALIGN_CENTER, -95, 0);
  lv_obj_remove_flag(map_nav_arrow, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_CHAIN));

  /* band + sign are painted by map_blit; hide the LVGL objects so
     they don't streak — their LABELS (children) stay visible and are
     repositioned onto the painted areas. */
  lv_obj_add_flag(map_nav_panel, LV_OBJ_FLAG_HIDDEN);

  lv_obj_add_flag(map_nav_arrow, LV_OBJ_FLAG_HIDDEN);

  map_nav_dist = lv_label_create(map_screen);
  lv_label_set_text(map_nav_dist, "\u2014");
  lv_obj_set_style_text_font(map_nav_dist, &lv_font_montserrat_34, 0);
  lv_obj_set_style_text_color(map_nav_dist, C_TEXT, 0);
  /* distance right NEXT to the arrow (align_to keeps the gap constant
     regardless of the label width: '50 m' vs '1.2 km') */
  lv_obj_align_to(map_nav_dist, map_nav_arrow, LV_ALIGN_OUT_RIGHT_MID, 16, 0);
  lv_obj_remove_flag(map_nav_dist, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_CHAIN));
  lv_obj_add_flag(map_nav_dist, LV_OBJ_FLAG_HIDDEN);


  /* speed limit sign: painted by map_blit_limit_sign; only the
     NUMBER stays an LVGL label (small region inside the sign) */
  /* speed limit sign: white circle, red ring, number inside */
  map_limit = lv_obj_create(map_screen);
  lv_obj_remove_style_all(map_limit);
  lv_obj_set_size(map_limit, 64, 64);
  lv_obj_set_style_radius(map_limit, 32, 0);
  lv_obj_set_style_bg_color(map_limit, C_TEXT, 0);
  lv_obj_set_style_bg_opa(map_limit, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(map_limit, 7, 0);
  lv_obj_set_style_border_color(map_limit, C_BAD, 0);      /* red ring */
  /* ROUND display: the visible area is a circle (r=240 around 240/240).
     A TOP_RIGHT corner align would place the sign in the INVISIBLE
     corner region outside the circle — it rendered fine but could
     never be seen. Fixed coords: box centre (360,120) is fully inside
     the visible circle (verified geometrically). */
  lv_obj_set_pos(map_limit, 360 - 32, 120 - 32);
  lv_obj_remove_flag(map_limit, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_CHAIN));
  /* the SIGN is painted by map_blit; keep the LVGL object hidden
     (added AFTER creation — hiding earlier dereferenced NULL and
     crashed the boot, reset_reason=11) */
  lv_obj_add_flag(map_limit, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(map_limit, LV_OBJ_FLAG_HIDDEN);          /* shown when limit > 0 */

  map_limit_label = lv_label_create(map_screen);
  lv_label_set_text(map_limit_label, "50");
  lv_obj_set_style_text_font(map_limit_label, &lv_font_montserrat_34, 0);
  lv_obj_set_style_text_color(map_limit_label, C_BG, 0);   /* black on white */
  lv_obj_add_flag(map_limit_label, LV_OBJ_FLAG_IGNORE_LAYOUT);
  lv_obj_align_to(map_limit_label, map_limit, LV_ALIGN_CENTER, 0, 0);
  lv_obj_remove_flag(map_limit_label, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_CHAIN));
  lv_obj_add_flag(map_limit_label, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(map_limit_label, LV_OBJ_FLAG_HIDDEN);
}

/*---------------------------------------------------------------
 * S5 — info overlay screen (Arrived / Link lost; design.md)
 *--------------------------------------------------------------*/

static void create_info_screen(void) {
  info_screen = new_screen();

  info_title = lv_label_create(info_screen);
  lv_label_set_text(info_title, "Arrived \u2713");
  lv_obj_set_style_text_font(info_title, &lv_font_montserrat_34, 0);
  lv_obj_align(info_title, LV_ALIGN_CENTER, 0, 0);

  info_sub = lv_label_create(info_screen);
  lv_label_set_text(info_sub, "Hold touch to go back");
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

/* Render the current NavState on the MAP INFO BAR (top): maneuver
 * arrow + speed-limit sign. The old nav screen is gone — the map is
 * THE navigation view. (The distance number was removed on user
 * request — 'a number I can't make sense of'.) */
static void render_nav_from_state(void) {
  if (map_nav_arrow)
    lv_img_set_src(map_nav_arrow,
      maneuver_icon(g_nav.maneuver[0] ? g_nav.maneuver : "straight"));

  if (map_nav_dist) {
    char d[16];
    if (g_nav.dist_m > 0) {
      if (g_nav.dist_m >= 1000)
        snprintf(d, sizeof d, "%d.%d km", g_nav.dist_m / 1000, (g_nav.dist_m % 1000) / 100);
      else
        snprintf(d, sizeof d, "%d m", g_nav.dist_m);
    } else {
      strcpy(d, "\u2014");
    }
    lv_label_set_text(map_nav_dist, d);
    /* re-anchor after the text change: a wider/narrower label must stay
     glued to the RIGHT of the arrow (align_to, not a fixed offset) */
    lv_obj_align_to(map_nav_dist, map_nav_arrow, LV_ALIGN_OUT_RIGHT_MID, 16, 0);
  }
  /* speed limit: the SIGN is painted by map_blit_limit_sign (part of
     the frame); only the NUMBER is an LVGL label inside it. */
  /* the limit number is painted into the frame by render_map_display
     (map_blit_text); the LVGL label stays hidden forever — un-hiding
     it here made it race the panel scan again (flickering sign). */
}

/* Apply a freshly parsed NavState to the state machine + UI. */
static void on_nav_state(const NavState &st) {
  g_nav = st;
  have_nav = true;
  last_data_ms = millis();
  rx_ok_count++;

  /* Reroute: phone is recomputing — flash a status hint, stay on map.
   * During the boot splash: record state but DON'T switch screens —
   * the splash exit picks the right screen for the final app state. */
  static bool reroute_hint = false;
  if (st.msg_type == NAV_MSG_REROUTE) {
    if (app != APP_NAV) {
      app = APP_NAV;
      if (!g_splash_until_ms) goto_screen(map_screen, "map (reroute)");
    }
    reroute_hint = !reroute_hint;
    if (reroute_hint) toast("Recalculating route…", C_WARN);
    return;
  }

  switch (st.msg_type) {
    case NAV_MSG_BEEP:
      /* heartbeat: keeps the link alive; does not change state (protocol.md).
       * No toast — the app pings every 5 s while idle; a toast would
       * flicker over the home screen permanently. The link status is
       * visible via the BLE dot colour. */
      break;

    case NAV_MSG_NAV: {
      if (app != APP_NAV) {
        app = APP_NAV;
        if (!g_splash_until_ms) goto_screen(map_screen, "map (nav)");
      }
      render_nav_from_state();
      break;
    }

    case NAV_MSG_IDLE:
      app = APP_IDLE;
      state_until_ms = 0;
      if (!g_splash_until_ms) goto_screen(home_screen, "home (idle)");
      break;

    case NAV_MSG_ARRIVED:
      app = APP_ARRIVED;
      state_until_ms = millis() + ARRIVED_TIMEOUT_MS;
      if (info_title) {
        lv_label_set_text(info_title, "Arrived ✓");
        lv_obj_set_style_text_color(info_title, C_ACCENT, 0);
        lv_label_set_text(info_sub, "Hold touch to go back");
      }
      if (!g_splash_until_ms) goto_screen(info_screen, "arrived");
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

  /* splash → the screen matching the CURRENT app state. If nav frames
   * arrived during the splash (app already NAV), the map is the right
   * screen — the old unconditional home switch is what made 'no map
   * data' look like a transfer problem. */
  if (g_splash_until_ms && now >= g_splash_until_ms) {
    g_splash_until_ms = 0;
    lv_obj_t *target = home_screen;
    const char *why = "home (splash done)";
    if (app == APP_NAV)        { target = map_screen;  why = "map (splash done, nav)"; }
    else if (app == APP_ARRIVED) { target = info_screen; why = "info (splash done, arrived)"; }
    active_screen = target;
    lv_scr_load(target);               /* hard switch — no fade ref to splash */
    if (g_splash_screen) {
      lv_obj_delete_async(g_splash_screen);   /* free the splash (160 KB) */
      g_splash_screen = NULL;
    }
    Serial.printf("[UI] -> %s\n", why);
  }

  /* NAV → LOST: link silence while navigating */
  if (have_nav && app == APP_NAV && now - last_data_ms > LINK_TIMEOUT_MS) {
    app = APP_LOST;
    state_until_ms = now + LOST_TIMEOUT_MS;
    if (info_title) {
      lv_label_set_text(info_title, "Connection lost");
      lv_obj_set_style_text_color(info_title, C_BAD, 0);
      lv_label_set_text(info_sub, "Waiting for phone…");
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
 * BLE → app callbacks (run on the COMM task via ble_link_poll).
 * Parse only — publish to the display task, never touch LVGL.
 *--------------------------------------------------------------*/

/* NavData JSON (protocol.md §2) — COMM thread. Parses and publishes the
   state to the display task (ui_q); no LVGL calls here. */
static void on_ble_nav(const void *data, size_t len) {
  char buf[NAV_MAX_LEN + 1];
  if (len >= sizeof(buf)) len = sizeof(buf) - 1;
  memcpy(buf, data, len);
  buf[len] = 0;

  rx_count++;
  NavState st;
  if (parse_nav_message(buf, len, &st)) {
    Serial.printf("[RX] BLE frame #%u OK  t=%d %u B  limit=%d dist=%d %s\n",
                  (unsigned)rx_count, (unsigned)st.msg_type, (unsigned)len,
                  st.limit_kmh, st.dist_m, st.maneuver);
    struct ui_msg m;
    m.kind = UI_MSG_NAV;
    m.nav  = st;
    ui_push(&m);
  } else {
    /* spec §6: parse error → frame ignored, rx_count still incremented.
     * On-board: log length + hex dump so we can read it without a wire. */
    Serial.printf("[RX] BLE frame #%u PARSE ERROR (%u B)\n",
                  (unsigned)rx_count, (unsigned)len);
    char dbg[96];
    const uint8_t *b = (const uint8_t *)buf;
    int n = len < 28 ? (int)len : 28;
    int p = 0;
    for (int i = 0; i < n && p < 80; i++)
      p += snprintf(dbg + p, sizeof(dbg) - p, "%02X ", b[i]);
    Serial.printf("[RX]   %s\n", dbg);
  }
  publish_status();
}

/* MapData binary frame (protocol.md §4/§6) — decode + render.
 * Runs on the app core (ble_link_poll drains the queue). */

/* per-line point buffers: mutable so lv_line keeps our pointer (no malloc at 2 Hz) */

/* ---- Dead reckoning between frames --------------------------------
 * Real frames arrive at 5 Hz (200 ms); the display paints 11-15 fps.
 * Between two real frames the world DRIFTS DOWN by the distance the
 * vehicle covered: dr_tick (75 ms) repaints the last geometry with a
 * growing offset and memcpys it to the panel — the map appears to
 * move at the repaint rate (~13 fps) instead of the transfer rate.
 * A real frame resets the offset (its geometry is exact). */
static void dr_repaint(const MapRenderFrame *rf, int dy);
static bool     g_dr_have = false;
static int32_t  g_dr_dy = 0;      /* accumulated drift since the last REAL frame */
static uint32_t g_dr_last_ms = 0;
static int32_t  g_dr_px_s = 0;             /* world drift, px/s */
#define DR_TICK_MS 75

/* full repaint of the last frame with offset dy — same composition as
   render_map_display (band, sign, markers, texts) so the picture is
   complete; only the world lines move. */
static void dr_repaint(const MapRenderFrame *rf, int dy) {
  static uint16_t *panel_fb = nullptr;
  if (panel_fb == nullptr) panel_fb = (uint16_t *)gfx->getFramebuffer();
  if (!panel_fb || !g_map_fb || !rf) return;
  map_blit_init(g_map_fb);
  map_blit_frame_dy(rf, dy);
  map_blit_band();
  map_blit_limit_sign(g_nav.limit_kmh > 0);
  if (rf->show_vehicle)
    map_blit_vehicle(vehicle_icon_map, MAP_CX, MAP_CY, 64);
  if (rf->marker_visible)
    map_blit_marker(rf->marker_x, rf->marker_y + dy);
  /* maneuver icon + distance: repainted identically (band content
     does not drift with the world) */
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
  memcpy(panel_fb, g_map_fb, (size_t)SCREEN_W * SCREEN_H * 2);
}

static void dr_tick_cb(lv_timer_t *t) {
  (void)t;
  if (!g_dr_have || active_screen != map_screen) return;
  if (g_nav.speed_kmh <= 0) return;         /* standing: nothing drifts */
  if (g_map_mux == NULL) return;
  uint32_t now = millis();
  if (!g_dr_last_ms) { g_dr_last_ms = now; return; }
  uint32_t dt = now - g_dr_last_ms;
  g_dr_last_ms = now;
  if (dt == 0 || dt > 500) return;

  /* snapshot the prepared frame under the mutex (short copy of the
     header fields is not enough — the lines embed their points, so we
     paint INSIDE the mutex; the comm task only holds it for its own
     prepare, and our paint is ~40 ms — acceptable contention) */
  xSemaphoreTake(g_map_mux, portMAX_DELAY);
  /* px/s = speed/3.6 m/s * scale/100 px/m */
  g_dr_px_s = (int32_t)(((int64_t)g_nav.speed_kmh * g_map_out.scale) / 360);
  g_dr_dy += (int32_t)(((int64_t)g_dr_px_s * dt) / 1000);
  if (g_dr_dy > 120 || g_dr_dy < -30) g_dr_dy = 0;   /* stale/crazy → snap */
  dr_repaint(&g_map_out, g_dr_dy);
  xSemaphoreGive(g_map_mux);
}


static void render_map_display(const MapRenderFrame *rf) {
  /* full-frame path: paint the geometry into the PSRAM framebuffer with
     our own fat-line renderer (map_blit), then invalidate the ONE image
     object — LVGL redraws the map region from the buffer, no line
     rasterization, no per-object invalidation. */
  static uint16_t *panel_fb = nullptr;
  if (panel_fb == nullptr) panel_fb = (uint16_t *)gfx->getFramebuffer();
  if (!panel_fb || !g_map_fb) {
    Serial.println("[MAP] render skipped: no fb!");
    return;
  }
  /* frame-rate diagnostics: the LVGL perf overlay is meaningless now
     (LVGL doesn't render the map), so we count OUR frames here and
     report once per 10 s over serial. */
  static uint32_t fps_frames = 0;
  static uint32_t fps_window_ms = 0;
  static uint32_t fps_last_ms = 0;
  {
    uint32_t now = millis();
    if (fps_last_ms == 0) fps_last_ms = now;
    fps_frames++;
    fps_window_ms = now - fps_last_ms;
    if (fps_window_ms >= 10000) {
      Serial.printf("[FPS] map: %u.%u fps (frame time %u ms avg)\n",
                    (unsigned)(fps_frames * 10000 / fps_window_ms),
                    (unsigned)((fps_frames * 100000 / fps_window_ms) % 10),
                    (unsigned)(fps_window_ms / fps_frames));
      fps_frames = 0;
      fps_last_ms = now;
    }
  }

  /* dead reckoning: a REAL frame is exact — drop the accumulated
     drift ENTIRELY (the tick kept a stale offset after real frames
     and fought them: the map jumped up/down repeatedly until the
     snap limit fired — user-visible as bouncing jumps) */
  g_dr_have = true;
  g_dr_last_ms = 0;
  g_dr_dy = 0;

  /* 1. paint OFFSCREEN into the PSRAM fb — the panel never shows the
        intermediate black/cleared state (that was the flicker). */
  map_blit_init(g_map_fb);
  map_blit_frame(rf);
  /* 2. band + speed-limit sign painted INTO the frame — as part of
        the frame they cannot flicker (the old LVGL objects raced the
        map memcpy every frame and showed as pixel streaks). */
  map_blit_band();
  map_blit_limit_sign(g_nav.limit_kmh > 0);
  /* 3. vehicle + destination markers painted INTO the map (alpha
        blended) — LVGL objects would leave black boxes over the map. */
  if (rf->show_vehicle)
    map_blit_vehicle(vehicle_icon_map, MAP_CX, MAP_CY, 64);
  if (rf->marker_visible)
    map_blit_marker(rf->marker_x, rf->marker_y);

  /* 3b. texts painted INTO the frame too (the last LVGL labels raced
        the panel scan and flickered as pixel streaks):
        - maneuver icon (48x48, alpha) + distance in the band
        - limit number inside the painted sign
        - heading/scale caption top */
  if (g_nav.limit_kmh > 0) {
    char s[8];
    snprintf(s, sizeof s, "%d", g_nav.limit_kmh);
    const lv_font_t *f = (g_nav.limit_kmh >= 100) ? &lv_font_montserrat_24
                           : &lv_font_montserrat_34;
    /* centred in the painted sign at (360,120) */
    const int w = (int)strlen(s) * (f->line_height / 2);
    map_blit_text_lv((void *)f, s, 360 - w / 2, 120 - f->line_height / 2, 0x0000);
  }
  {
    /* distance + maneuver icon: centred as a GROUP in the band (the
       user's original layout: icon left of the text, pair centred) */
    char d[16];
    if (g_nav.dist_m >= 1000) {
      /* one decimal, e.g. 1050 → "1.0 km" */
      snprintf(d, sizeof d, "%d.%d km", g_nav.dist_m / 1000,
               (g_nav.dist_m % 1000) / 100);
    } else if (g_nav.dist_m > 0) {
      snprintf(d, sizeof d, "%d m", g_nav.dist_m);
    } else {
      snprintf(d, sizeof d, "--");
    }
    const int band_y = SCREEN_H - 84;   /* band top */
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
  /* scale caption: REMOVED per user request — the zoom level is
     obvious from the roads, nobody needs metres numbers while riding. */
  /* 3. ONE block copy into the panel framebuffer the hardware scans.
        ~450 KB memcpy ≈ 15-25 ms — atomic enough visually (no black
        flash; worst case one torn frame line). */
  memcpy(panel_fb, g_map_fb, (size_t)SCREEN_W * SCREEN_H * 2);
  /* 4. NOTHING left for LVGL on the map screen: band, sign, markers,
        maneuver icon, distance, limit number and the caption are all
        painted into the frame. No LVGL region ever races the panel
        scan — the flicker is gone by construction. */
  /* DIRECT mode: do NOT invalidate anything here — the map pixels are
     already in the fb. Invalidating the screen would make LVGL refill
     the region with the screen bg (erasing the map). Overlays
     invalidate themselves when their content changes (labels, band,
     sign) — LVGL redraws only those small regions. The map area
     between/around the overlays is never touched by LVGL. */

  /* ---- destination dot marker (screen coords, pre-rotated) -------- */
  if (map_marker) {
    if (rf->marker_visible) {
      lv_obj_remove_flag(map_marker, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_pos(map_marker, rf->marker_x - 12, rf->marker_y - 12);
    } else {
      lv_obj_add_flag(map_marker, LV_OBJ_FLAG_HIDDEN);
    }
  }

  /* vehicle + destination markers are painted INTO the map by
     map_blit (alpha-blended, no black boxes); the LVGL objects stay
     hidden on the map screen. */
  if (map_vehicle) lv_obj_add_flag(map_vehicle, LV_OBJ_FLAG_HIDDEN);
  if (map_marker) lv_obj_add_flag(map_marker, LV_OBJ_FLAG_HIDDEN);

  /* ---- scale bar + caption: painted into the frame by map_blit_text_lv
     (see above). The LVGL objects stay hidden FOREVER — un-hiding them
     here made them race the panel scan (the flickering box at the top
     edge was this label being re-shown every frame). */
  if (map_scale) lv_obj_add_flag(map_scale, LV_OBJ_FLAG_HIDDEN);
  if (map_label) lv_obj_add_flag(map_label, LV_OBJ_FLAG_HIDDEN);
}

/* Display thread: if a new prepared frame is published AND the map
   screen is active, blit it. The blit is short (pool updates only —
   the actual flush happens later in lv_timer_handler), so holding
   g_map_mux across it only delays the comm task's next publish by a
   few ms. */
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

/* MapData binary frame (protocol.md §4/§6) — COMM thread.
   Parse + geometry (prepare_map_frame), then publish the prepared frame
   to the display task. No LVGL calls here. */
static void on_ble_map_frame(const void *data, size_t len) {
  const uint8_t *f = (const uint8_t *)data;

  if (len > MAP_FRAME_MAX) {
    map_drop_count++;
    Serial.println("[MAP] drop: > 4 KB");
    publish_status();
    return;
  }

  /* decode first (bounds-checks all bytes, returns false on any error).
     g_map_parse_mux: the serial mock (display task) can call this handler
     concurrently with a real BLE frame (comm task) — both share the
     static fr below. */
  static MapFrame fr;   /* ~7 KB — lives in .bss, off the comm-task stack */
  xSemaphoreTake(g_map_parse_mux, portMAX_DELAY);
  if (!parse_map_frame(f, len, &fr)) {
    xSemaphoreGive(g_map_parse_mux);
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

  /* geometry → publish. The display task blits it on its next loop
     iteration (map_blit_if_new) when the map screen is active. */
  xSemaphoreTake(g_map_mux, portMAX_DELAY);
  prepare_map_frame(&fr, &g_map_out);
  g_map_ready = true;
  xSemaphoreGive(g_map_mux);
  xSemaphoreGive(g_map_parse_mux);

  publish_status();
}

/* Control JSON (protocol.md §5) — COMM thread. reset_map is pure comm
   state (handled here); brightness / force_idle are UI effects and get
   published to the display task. */
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
  if (cs.reset_map) {
    map_seq_last = 0;
    map_seq_seen = false;
    map_drop_count = 0;
    Serial.println("[CTL] map buffer reset (seq → 0)");
  }
  if (cs.brightness_set || cs.force_idle) {
    struct ui_msg m;
    m.kind  = UI_MSG_CTRL;
    m.ctrl  = cs;
    ui_push(&m);
  }
  publish_status();
}

/*---------------------------------------------------------------
 * Display-task side of the hand-off.
 *--------------------------------------------------------------*/

/* Apply a Control message (DISPLAY thread). brightness → LEDC + NVS,
   force_idle → state machine + screen. */
static void apply_ctrl(const CtrlState *cs) {
  if (cs->brightness_set) {
    apply_brightness(cs->brightness_pct);   /* also persisted (spec §5) */
    Serial.printf("[CTL] brightness %d%%\n", cs->brightness_pct);
  }
  if (cs->force_idle) {
    app = APP_IDLE;
    state_until_ms = 0;
    goto_screen(home_screen, "home (ctrl)");
    Serial.println("[CTL] state → idle");
  }
}

/* Drain the ui_q ring (DISPLAY thread). Nav → state machine + UI,
   Ctrl → apply_ctrl. Runs every loop iteration, so the ring (16 slots)
   never fills under normal 1–2 Hz traffic. */
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
      on_nav_state(m.nav);
      publish_status();
    } else if (m.kind == UI_MSG_CTRL) {
      apply_ctrl(&m.ctrl);
      publish_status();
    }
  }
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
    /* NOT connected: blink amber — the old dark grey (0x3B4148) was
       invisible on the black home screen, so the connection status
       showed NOTHING before the first connect (user report #3). */
    on = !on;
    if (ble_dot) lv_obj_set_style_bg_color(ble_dot, on ? C_WARN : lv_color_hex(0x7A5A00), 0);
    return;
  }

  if (app == APP_IDLE) {
    /* connected but NO data flowing: blink GREEN, both phases clearly
       visible. The old amber <-> 0x3B4148 blink read as a trembling
       point, not a blink — the dark grey phase was invisible on the
       black screen (user: 'the blink just doesn't happen'). */
    on = !on;
    if (ble_dot) lv_obj_set_style_bg_color(ble_dot,
        on ? C_ACCENT : lv_color_hex(0x0A4A0F), 0);
  } else {
    if (ble_dot) lv_obj_set_style_bg_color(ble_dot, (app == APP_LOST) ? C_BAD : C_ACCENT, 0);
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
  /* hard switch, NO fade: in DIRECT mode with a single buffer the fade
     needs a full-screen snapshot of the outgoing screen; when that
     fails the whole map area turns white. A hard cut is instant and
     safe (screen switches are rare: nav start/stop, taps). */
  lv_scr_load(scr);
  Serial.printf("[UI] -> %s\n", name);
}

/* Serial input line buffer (nav frames can exceed 100 bytes — generous budget) */
#define SER_LINE_MAX 512

static void handle_serial(void) {
  static char line[SER_LINE_MAX];
  static int li = 0;

  while (Serial.available()) {
    char c = Serial.read();
    /* temporary diagnostic — do NOT commit */

    if (c == '\n' || c == '\r') {
      if (li == 0) continue;
      line[li] = 0;
      /* trim trailing whitespace */
      while (li > 0 && (line[li-1] == ' ' || line[li-1] == '\t')) line[--li] = 0;
      li = 0;  /* reset for the next line */
      if (!line[0]) continue;

      if (line[0] == 'h')                 goto_screen(home_screen, "home");
      else if (line[0] == 'n')            goto_screen(map_screen, "map");
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
  if      (active_screen == home_screen) goto_screen(map_screen, "map (tap)");
  else                                 goto_screen(home_screen, "home (tap)");
}

static void handle_gesture_state(void) {
  /* during the boot splash: ignore input — no screen switching away
   * from a screen that then gets deleted under the fade animation */
  if (g_splash_until_ms) return;

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
 * COMM task — BLE + parse + geometry (NO LVGL).
 *
 * Runs the same work the main loop used to do for the radio:
 * drain the BLE queues (ble_link_poll), parse NavData/MapData/
 * Control and run the map geometry stage. Results are published
 * to the display task via g_map_out + ui_q (see hand-off block).
 *
 * Stack: the parsers keep their big buffers in .bss (static), so
 * 4 KB is comfortable headroom on top of the FreeRTOS overhead.
 * Priority: 1 = same as the display task (Arduino core). Time-sliced,
 * so a geometry burst shares the CPU with the screen instead of
 * preempting it — the display never gets starved by an incoming
 * map frame. The comm task also yields on every queue wait.
 *--------------------------------------------------------------*/
static void comm_task(void *arg) {
  (void)arg;
  for (;;) {
    ble_link_poll();   /* drain NavData/Control/MapData, adv state */
    vTaskDelay(pdMS_TO_TICKS(5));
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

  /* Allocate the 480×480 RGB565 map framebuffer EARLY (setup order):
     in DIRECT mode it doubles as the LVGL display framebuffer, and
     create_map_screen only wires the map_blit renderer to it. */
  size_t spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  Serial.printf("[MEM] SPIRAM free: %u KB\n", (unsigned)(spiram_free / 1024));
  if (spiram_free == 0) {
    Serial.println("[MEM] NO SPIRAM! Board option must be 'PSRAM: OPI PSRAM'");
  }
  g_map_fb = (uint16_t *)heap_caps_malloc(
      sizeof(uint16_t) * SCREEN_W * SCREEN_H, MALLOC_CAP_SPIRAM);
  if (g_map_fb && map_blit_init(g_map_fb)) {
    for (size_t i = 0; i < (size_t)SCREEN_W * SCREEN_H; i++) g_map_fb[i] = 0;
    Serial.printf("[MEM] map fb: PSRAM %u KB\n",
                  (unsigned)(SCREEN_W * SCREEN_H * 2 / 1024));
  } else {
    g_map_fb = NULL;
    Serial.printf("[MEM] map fb alloc FAILED (wanted %u KB, SPIRAM free %u KB)\n",
                  (unsigned)(SCREEN_W * SCREEN_H * 2 / 1024),
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
  }

  /* LVGL render buffer: ONE buffer in INTERNAL RAM. Pixel access from
     SPIRAM is several times slower and the render loop is the FPS
     bottleneck (measured 1-2 FPS with PSRAM buffers). ONE 46 KB buffer
     (not two): the BLE/NimBLE stack needs a large share of internal
     RAM — two buffers starved it and the device bootlooped with
     'hci inits failed'. Single-buffer = LVGL waits for the flush to
     finish before rendering the next chunk (no tearing protection,
     irrelevant for our use). PSRAM stays as fallback. */
  /* DIRECT mode: the 480×480 PSRAM map framebuffer (allocated in
     create_map_screen, before lv_display_create runs) IS the display
     framebuffer. LVGL draws the overlays straight into it and the
     flush sends dirty regions to the panel — the map image is never
     copied through the draw pipeline again (that copy was ~95 ms).
     The map_blit renderer paints the map directly into the same
     buffer; after painting we invalidate the map area.
     Fallback: if the PSRAM fb is missing, PARTIAL mode with a 48-row
     internal buffer (slow old path). */
  if (g_map_fb) {
    buf1 = (uint8_t *)g_map_fb;
    buf2 = nullptr;
    Serial.println("[MEM] DIRECT mode: PSRAM map fb = display fb");
  } else {
    size_t buffer_size = sizeof(uint16_t) * screen_width * 48;
    buf1 = (uint8_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_INTERNAL);
    if (!buf1) buf1 = (uint8_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM);
    buf2 = nullptr;
    Serial.println("[MEM] fallback: PARTIAL mode");
  }
  if (!buf1) {
    Serial.println("[FATAL] LVGL framebuffer allocation failed");
    while (true) delay(1000);
  }

  lv_display_t *display = lv_display_create(screen_width, screen_height);
  lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(display, my_disp_flush);
  if (g_map_fb) {
    lv_display_set_buffers(display, buf1, buf2,
                           (uint32_t)screen_width * screen_height * 2,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
  } else {
    lv_display_set_buffers(display, buf1, buf2,
                           sizeof(uint16_t) * screen_width * 48,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
  }

  lv_indev_t *touch_indev = lv_indev_create();
  lv_indev_set_type(touch_indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(touch_indev, my_touchpad_read);
  lv_indev_set_display(touch_indev, display);

  /* ---- splash FIRST: paint the logo BEFORE the remaining boot work
   * (screen creation, BLE GATT init) — otherwise the splash never
   * shows: rendering normally starts only in loop(). One manual
   * lv_timer_handler() here flushes the splash to the panel now. */
  /* Splash WITHOUT LVGL: in DIRECT mode the lv_timer_handler() here
     hung the boot (full-screen render through the draw pipeline into
     the 450 KB PSRAM buffer — never returned; boot stuck on the logo).
     Paint the splash straight into the PANEL framebuffer instead:
     fill black + blit the I8 logo (palette + indices) — a few ms. */
  {
    uint16_t *panel_fb0 = (uint16_t *)gfx->getFramebuffer();
    if (panel_fb0) {
      for (size_t i = 0; i < (size_t)SCREEN_W * SCREEN_H; i++) panel_fb0[i] = 0x0000;
      /* logo: 480x188 I8 — palette (256 ARGB8888, B,G,R,A) + indices */
      const uint8_t *pal = libremoto_logo_map;
      const uint8_t *idx = libremoto_logo_map + 256 * 4;
      const int lw = 480, lh = 188;
      const int x0 = (SCREEN_W - lw) / 2, y0 = (SCREEN_H - lh) / 2;
      for (int y = 0; y < lh; y++) {
        uint16_t *dst = panel_fb0 + (size_t)(y0 + y) * SCREEN_W + x0;
        const uint8_t *row = idx + (size_t)y * lw;
        for (int x = 0; x < lw; x++) {
          const uint8_t *e = pal + row[x] * 4;   /* B,G,R,A */
          const uint8_t b = e[0], g = e[1], r = e[2], a = e[3];
          if (a < 16) { dst[x] = 0x0000; continue; }   /* transparent */
          /* panel byte order: R/B swapped */
          dst[x] = (uint16_t)(((uint32_t)b >> 3) << 11 |
                              ((uint32_t)g >> 2) << 5 |
                              ((uint32_t)r >> 3));
        }
      }
      Serial.println("[UI] splash painted to panel fb");
    }
    /* no LVGL screen switch yet — home comes after the splash timeout
       via goto_screen(); active_screen stays NULL-safe */
    g_splash_until_ms = millis() + 2000;
  }

  create_home_screen();
  create_map_screen();
  create_info_screen();

  /* ---- threading hand-off (comm task → display task) -------------
   * Mutexes must exist before the comm task starts publishing. */
  g_map_mux = xSemaphoreCreateMutex();
  g_ui_mux  = xSemaphoreCreateMutex();
  g_map_parse_mux = xSemaphoreCreateMutex();
  ui_free   = UI_Q_SIZE;
  if (!g_map_mux || !g_ui_mux || !g_map_parse_mux) {
    Serial.println("[FATAL] hand-off mutex creation failed");
    while (true) delay(1000);
  }

  /* ---- BLE GATT server (protocol.md §1) ----
   * Server runs in its own FreeRTOS task; callbacks only push into
   * mutex-protected queues that ble_link_poll() drains — now on the
   * COMM task, which parses + prepares and publishes to the display. */
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

  /* ---- COMM task (BLE + parse + geometry) ------------------------ */
  xTaskCreate(comm_task, "comm", 4096, NULL, 1, NULL);
  Serial.println("[TASK] comm task started (BLE + parse + geometry)");

  ble_timer = lv_timer_create(ble_blink_cb, 500, NULL);
  lv_timer_create(dr_tick_cb, DR_TICK_MS, NULL);   /* dead-reckoning repaints */

  /* NO goto_screen(home_screen) here — the splash owns the display
   * until g_splash_until_ms expires (handle_state_timeouts switches to
   * home). The old unconditional switch right here is what made the
   * splash invisible: it overwrote the splash before loop() ever
   * rendered a frame. */
  Serial.println("Libre-Moto ready. Gestures: tap=cycle, hold=home, swipe=brightness");
  Serial.println("Serial cmds: h n m | b<0-100> | {\"v\":1,\"t\":...} (protocol.md §2)");
}

/* render timing diagnostics: where do the ~160 ms full redraws go?
 *  - t_blit  = render_map_display (setting 44 lines' points)
 *  - t_lvgl  = lv_timer_handler (render into buffer + flush to panel)
 *  - t_flush = my_disp_flush sum (panel transfer only)
 * printed once per second; maxima over that second. */
void loop() {
  /* DISPLAY task: everything LVGL lives here. The comm task
     (BLE + parse + geometry) publishes into g_map_out / ui_q. */
  handle_serial();          /* debug channel (serial stays on display) */
  drain_ui_queue();         /* apply parsed Nav/Ctrl from the comm task */
  if (g_splash_until_ms) {
    /* boot splash still owns the panel (painted directly into the panel
       fb in setup()). LVGL has no screen loaded yet: the first
       lv_timer_handler() would render the DEFAULT screen (white bg)
       and flush it over the logo — the 'logo -> white -> icon' boot
       the user saw. Hold off until the splash expires; the switch in
       handle_state_timeouts loads home first. */
    handle_state_timeouts();     /* expires the splash -> loads home */
    delay(20);
    return;
  }
  map_blit_if_new();        /* blit the latest prepared map frame */
  handle_gesture_state();
  handle_state_timeouts();
  lv_timer_handler();
  delay(5);
}
