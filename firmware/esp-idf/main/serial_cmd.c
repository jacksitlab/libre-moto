/* serial_cmd.c — serial debug channel for ESP-IDF.
 *
 * The CrowPanel has NO USB-UART bridge chip: the USB-C connector is
 * wired to the ESP32-S3's native USB Serial/JTAG controller (the same
 * port the Arduino build used via "USB CDC on Boot"). Logs and the
 * debug commands therefore run over the USB-SERIAL-JTAG driver, not
 * UART0 (whose pins only exist on the expansion connector).
 *
 * Same commands as the sketch's handle_serial():
 *   h / n / m       → home / map jump
 *   b / b2          → full-screen benchmark
 *   b<0-100>        → brightness
 *   mt0..mt3        → MapData self-test frames
 *   {"v":1,...}     → NavData JSON frame
 */

#include "serial_cmd.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "driver/usb_serial_jtag.h"

static const char *TAG = "serial";

#define SER_LINE_MAX 512

void serial_cmd_init(void) {
    /* ESP_LOG already mirrors to USB-SERIAL-JTAG via the secondary console
     * (CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG). This driver install
     * adds the RX side so the debug commands can be typed in idf.py monitor. */
    if (usb_serial_jtag_is_connected()) {
        ESP_LOGI(TAG, "USB Serial/JTAG connected");
    }
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 2048;
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "USB Serial/JTAG driver install failed: %s",
                 esp_err_to_name(err));
    }
}

void serial_cmd_poll(void) {
    static char line[SER_LINE_MAX];
    static int li = 0;

    char buf[64];
    int n = usb_serial_jtag_read_bytes(buf, sizeof buf, 0);
    for (int i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\n' || c == '\r') {
            if (li == 0) continue;
            line[li] = 0;
            while (li > 0 && (line[li-1] == ' ' || line[li-1] == '\t')) line[--li] = 0;
            li = 0;
            if (!line[0]) continue;
            serial_cmd_handle_line(line);
        } else if (li < SER_LINE_MAX - 1) {
            line[li++] = c;
        }
    }
}
