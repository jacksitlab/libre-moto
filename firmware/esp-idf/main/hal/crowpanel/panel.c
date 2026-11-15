/* panel_st7701.c — CrowPanel 2.1" RGB panel (ST7701S 480x480) for ESP-IDF.
 *
 * Native port of the Arduino_GFX driver chain the sketch used:
 *   Arduino_SWSPI (3-wire bitbang: CS/SCK/SDA, no DC — ST7701 uses
 *   9-bit SPI but the init list only needs 8-bit commands) +
 *   Arduino_ESP32RGBPanel (RGB parallel, DMA bounce buffers) +
 *   st7701_type5_init_operations.
 *
 * The panel framebuffer is allocated by esp_lcd in PSRAM (fb_in_psram);
 * platform_panel_get_framebuffer() returns the DMA-scanned buffer, the
 * same role gfx->getFramebuffer() played in the sketch.
 */

#include "panel.h"

#include "config.h"

#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_idf_version.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"

static const char *TAG = "panel";

/* --- 3-wire bitbang SPI (init commands only) ------------------ */
static gpio_num_t s_cs, s_sck, s_sda;

static inline void bb_delay(void) {
    /* ~100 kHz-ish: ST7701 init tolerates a wide range; Arduino_GFX
     * bit-bangs at similar speed via digitalWrite (~200 ns edges). */
    for (volatile int i = 0; i < 20; i++) {
        __asm volatile("nop");
    }
}

static void bb_write_bit(int bit) {
    gpio_set_level(s_sck, 0);
    bb_delay();
    gpio_set_level(s_sda, bit ? 1 : 0);
    bb_delay();
    gpio_set_level(s_sck, 1);
    bb_delay();
}

/* Send one byte, MSB first, prefixed by the 9th D/C bit.
 *
 * The CrowPanel wires the ST7701 as 3-wire 9-BIT SPI (no D/C line):
 * every byte is preceded by one D/C bit — LOW = command, HIGH = data.
 * This mirrors Arduino_SWSPI::WRITE9BITCOMMAND/WRITE9BITDATA exactly
 * (D/C bit first, then the byte MSB-first). An 8-bit-only bitbang
 * sends the whole init stream as garbage — the panel never turns on.
 */
static void bb_write_byte9(uint8_t b, int dc) {
    /* D/C bit */
    gpio_set_level(s_sck, 0);
    bb_delay();
    gpio_set_level(s_sda, dc ? 1 : 0);
    bb_delay();
    gpio_set_level(s_sck, 1);
    bb_delay();
    /* the 8 payload bits */
    for (int i = 7; i >= 0; i--) bb_write_bit((b >> i) & 1);
}

static void bb_write_cmd(uint8_t cmd) {
    gpio_set_level(s_cs, 0);
    bb_write_byte9(cmd, 0);
    gpio_set_level(s_cs, 1);
}

static void bb_write_data(uint8_t data) {
    gpio_set_level(s_cs, 0);
    bb_write_byte9(data, 1);
    gpio_set_level(s_cs, 1);
}

/* --- ST7701 type5 init sequence (from Arduino_RGB_Display.h) --- */
/* Flat command stream: {cmd, n, data...}, terminated by {0x11, 0} (sleep
 * out + delay) followed by {0x00, 0x00}. */
static const uint8_t ST7701_INIT_STREAM[] = {
    0xFF, 5, 0x77, 0x01, 0x00, 0x00, 0x10,     /* switch to command page 0x10 */
    0xC0, 2, 0x3B, 0x00,
    0xC1, 2, 0x0B, 0x02,
    0xC2, 2, 0x00, 0x02,
    0xCC, 1, 0x10,
    0xCD, 1, 0x08,
    0xB0, 16, 0x02, 0x13, 0x1B, 0x0D, 0x10, 0x05, 0x08, 0x07,
              0x07, 0x24, 0x04, 0x11, 0x0E, 0x2C, 0x33, 0x1D,
    0xB1, 16, 0x05, 0x13, 0x1B, 0x0D, 0x11, 0x05, 0x08, 0x07,
              0x07, 0x24, 0x04, 0x11, 0x0E, 0x2C, 0x33, 0x1D,
    0xFF, 5, 0x77, 0x01, 0x00, 0x00, 0x11,     /* page 0x11 */
    0xB0, 1, 0x5D,
    0xB1, 1, 0x43,                            /* VCOM amplitude */
    0xB2, 1, 0x81,                            /* VGH 12V */
    0xB3, 1, 0x80,
    0xB5, 1, 0x43,                            /* VGL -8.3V */
    0xB7, 1, 0x85,
    0xB8, 1, 0x20,
    0xC1, 1, 0x78,
    0xC2, 1, 0x78,
    0xD0, 1, 0x88,
    0xE0, 3, 0x00, 0x00, 0x02,
    0xE1, 11, 0x03, 0xA0, 0x00, 0x00, 0x04, 0xA0, 0x00, 0x00, 0x00, 0x20, 0x20,
    0xE2, 13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xE3, 4, 0x00, 0x00, 0x11, 0x00,
    0xE4, 2, 0x22, 0x00,
    0xE5, 16, 0x05, 0xEC, 0xA0, 0xA0, 0x07, 0xEE, 0xA0, 0xA0,
              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xE6, 4, 0x00, 0x00, 0x11, 0x00,
    0xE7, 2, 0x22, 0x00,
    0xE8, 16, 0x06, 0xED, 0xA0, 0xA0, 0x08, 0xEF, 0xA0, 0xA0,
              0x00, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00,
    0xEB, 7, 0x00, 0x00, 0x40, 0x40, 0x00, 0x00, 0x00,
    0xED, 16, 0xFF, 0xFF, 0xFF, 0xBA, 0x0A, 0xBF, 0x45, 0xFF,
              0xFF, 0x54, 0xFB, 0xA0, 0xAB, 0xFF, 0xFF, 0xFF,
    0xEF, 6, 0x10, 0x0D, 0x04, 0x08, 0x3F, 0x1F,
    0xFF, 5, 0x77, 0x01, 0x00, 0x00, 0x13,     /* page 0x13 */
    0xEF, 1, 0x08,
    0xFF, 5, 0x77, 0x01, 0x00, 0x00, 0x00,     /* page 0x00 */
    0x36, 1, 0x08,                            /* MADCTL: BGR (kept from sketch) */
    0x3A, 1, 0x60,                            /* RGB666 on the wire */
    0x11, 0,                                  /* sleep out */
    0x00, 0,                                  /* (stream terminator handled below) */
};

static esp_lcd_panel_handle_t s_panel = NULL;
static uint16_t *s_fb = NULL;

/* Run the init command stream. Format: cmd, n, n data bytes. */
static void st7701_run_init(const uint8_t *stream) {
    size_t i = 0;
    while (1) {
        uint8_t cmd = stream[i];
        uint8_t n = stream[i + 1];
        if (cmd == 0x11 && n == 0) {
            /* sleep out + required delay */
            bb_write_cmd(0x11);
            vTaskDelay(pdMS_TO_TICKS(120));
            i += 2;
            if (stream[i] == 0x00 && stream[i + 1] == 0x00) break;
            continue;
        }
        bb_write_cmd(cmd);
        for (uint8_t k = 0; k < n; k++) bb_write_data(stream[i + 2 + k]);
        i += 2 + n;
    }
    /* display on */
    bb_write_cmd(0x29);
    vTaskDelay(pdMS_TO_TICKS(50));
}

esp_err_t panel_init(esp_lcd_panel_handle_t *out_panel, uint16_t **out_fb) {
    /* 1. GPIOs for the init SPI (CS=16, SCK=2, SDA=1 — config.h pins) */
    s_cs = PIN_CS;
    s_sck = PIN_SCK;
    s_sda = PIN_SDA;
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_CS) | (1ULL << PIN_SCK) | (1ULL << PIN_SDA),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(s_cs, 1);
    gpio_set_level(s_sck, 1);
    gpio_set_level(s_sda, 1);

    /* 2. Run the type5 init (after the PCF8574 reset sequence, which the
       app runs BEFORE calling panel_init — see app_main.c boot order). */
    st7701_run_init(ST7701_INIT_STREAM);

    /* 3. RGB panel: same timing as the Arduino_ESP32RGBPanel config in the
       sketch (hsync 10/4/20, vsync 10/4/20, pclk 12 MHz, active-neg off). */
    esp_lcd_rgb_timing_t timing = {
        .pclk_hz = 12 * 1000 * 1000,
        .h_res = SCREEN_W,
        .v_res = SCREEN_H,
        .hsync_pulse_width = 4,
        .hsync_back_porch = 20,
        .hsync_front_porch = 10,
        .vsync_pulse_width = 4,
        .vsync_back_porch = 20,
        .vsync_front_porch = 10,
        .flags = {
            .hsync_idle_low = 0,
            .vsync_idle_low = 0,
            .de_idle_high = 0,
            .pclk_active_neg = 0,
            .pclk_idle_high = 0,
        },
    };

    /* Data pin map — THE ORDER IS B,G,R (not R,G,B!): esp_lcd shifts each
       RGB565 pixel out MSB-first over data_gpio_nums[0..15], so the HIGH
       5 bits (red) leave via index 0..4. The Arduino_GFX driver maps
       B[0..4] to indices 0..4 (Arduino_ESP32RGBPanel.cpp, non-big-endian
       branch) — the panel's B- pins sit where esp_lcd sends the R bits.
       Copying that exact mapping keeps R/B from swapping (orange->blue). */
    gpio_num_t data_pins[] = {
        PIN_B0, PIN_B1, PIN_B2, PIN_B3, PIN_B4,
        PIN_G0, PIN_G1, PIN_G2, PIN_G3, PIN_G4, PIN_G5,
        PIN_R0, PIN_R1, PIN_R2, PIN_R3, PIN_R4,
    };

    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = timing,
        .data_width = 16,              /* RGB565 */
        .num_fbs = 1,
        .bounce_buffer_size_px = SCREEN_W * 20,   /* two 20-line DMA bounce buffers, as the sketch */
        .hsync_gpio_num = PIN_HSYNC,
        .vsync_gpio_num = PIN_VSYNC,
        .de_gpio_num = PIN_DE,
        .pclk_gpio_num = PIN_PCLK,
        .disp_gpio_num = -1,
        .data_gpio_nums = {0},
        .flags = {
            .disp_active_low = 0,
            .refresh_on_demand = 0,
            .fb_in_psram = 1,          /* 450 KB fb in PSRAM, DMA-scanned */
            .double_fb = 0,
            .no_fb = 0,
            .bb_invalidate_cache = 0,
        },
    };
#if ESP_IDF_VERSION_MAJOR >= 6
    /* v6: bits_per_pixel/psram_trans_align were replaced by explicit
     * in/out color formats + dma_burst_size. data_width=16 already
     * implies RGB565 in both, so only the burst size needs setting. */
    cfg.in_color_format = LCD_COLOR_FMT_RGB565;
    cfg.out_color_format = LCD_COLOR_FMT_RGB565;
    cfg.dma_burst_size = 64;
#else
    cfg.bits_per_pixel = 16;
    cfg.psram_trans_align = 64;
#endif
    memcpy(cfg.data_gpio_nums, data_pins, sizeof(data_pins));

    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    /* Fetch the DMA frame buffer (the buffer the hardware scans). */
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(s_panel, 1, (void **)&s_fb));

    ESP_LOGI(TAG, "ST7701 480x480 RGB panel up, fb=%p (PSRAM)", s_fb);
    *out_panel = s_panel;
    *out_fb = s_fb;
    return ESP_OK;
}

uint16_t *panel_get_framebuffer(void) { return s_fb; }
