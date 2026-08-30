#ifndef __LIBRE_MOTO_CONFIG_H
#define __LIBRE_MOTO_CONFIG_H

/*------------------------------------------------------------
 * Libre-Moto — hardware & app configuration
 * (CrowPanel 2.1", ESP32-S3R8 — see docs/hardware.md)
 *-----------------------------------------------------------*/

/* ---- I2C bus (shared by PCF8574 expander + CST816 touch) -- */
#define I2C_SDA_PIN        38
#define I2C_SCL_PIN        39

/* ---- PCF8574 I/O expander (I2C address) ------------------- */
#define PCF8574_ADDR       0x21
#define PCF_TP_RESET       P0   /* touch (CST816) reset        */
#define PCF_TP_INT         P2   /* touch interrupt (input)     */
#define PCF_LCD_POWER      P3   /* LCD power                   */
#define PCF_LCD_RESET      P4   /* LCD (ST7701) reset          */
/* NOTE: board revision without rotary knob/encoder — all input is touch.
   PCF8574 P5 (was knob press) is unused. */

/* ---- Display ----------------------------------------------- */
#define SCREEN_W           480
#define SCREEN_H           480
#define SCREEN_BACKLIGHT_PIN 6
/* RGB panel pins (ST7701S, see docs/hardware.md) */
#define PIN_CS             16
#define PIN_SCK            2
#define PIN_SDA            1
#define PIN_DE             40
#define PIN_VSYNC          7
#define PIN_HSYNC          15
#define PIN_PCLK           41
#define PIN_R0             46
#define PIN_R1             3
#define PIN_R2             8
#define PIN_R3             18
#define PIN_R4             17
#define PIN_G0             14
#define PIN_G1             13
#define PIN_G2             12
#define PIN_G3             11
#define PIN_G4             10
#define PIN_G5             9
#define PIN_B0             5
#define PIN_B1             45
#define PIN_B2             48
#define PIN_B3             47
#define PIN_B4             21

#define PWM_FREQ           5000
#define PWM_RES            8

/* ---- Touch -------------------------------------------------- */
#define I2C_TOUCH_ADDR     0x15
#define TOUCH_Y_OFFSET     20    /* empirical offset from working demo */

/* ---- App ---------------------------------------------------- */
#define BLE_NAME           "LibreMoto"
#define BRIGHTNESS_STEPS   6
#define BRIGHTNESS_MIN_PCT 15

/* Firmware app state (see docs/design.md state machine) */
enum AppState { APP_IDLE, APP_NAV, APP_ARRIVED, APP_LOST };

/* Link timeout: >15 s without a valid NavData frame → LOST (protocol.md §2) */
#define LINK_TIMEOUT_MS    15000
/* ARRIVED: show "Arrived ✓" for 10 s, then → IDLE (protocol.md §2) */
#define ARRIVED_TIMEOUT_MS 10000
/* LOST: show "Link lost" for 5 s, then → IDLE (protocol.md §2) */
#define LOST_TIMEOUT_MS      5000

#endif
