#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "ssd1306.h"
#include "string.h"

/*
 * I2C Pin & Bus Configuration
 * ----------------------------
 * SCL (clock) and SDA (data) are the two wires of the I2C protocol.
 * GPIO 22 and 21 are the default I2C pins on most ESP32 dev boards.
 * I2C_MASTER_NUM selects which of the ESP32's two I2C peripherals to use (0 or 1).
 * 400000 Hz = "Fast Mode" I2C — the SSD1306 supports up to 400 kHz.
 * 0x3C is the default 7-bit I2C address of most SSD1306 OLED modules.
 */
#define I2C_MASTER_SCL_IO 22
#define I2C_MASTER_SDA_IO 21
#define I2C_MASTER_NUM 0
#define I2C_MASTER_FREQ_HZ 400000
#define SSD1306_I2C_ADDRESS 0x3C

/* TAG is used by ESP_LOGI / ESP_LOGE to label log output in the serial monitor.
   All messages from this file will be prefixed with "Hello_World". */
static const char *TAG = "Hello_World";

/* Global handle for the SSD1306 device.
 * This is an opaque pointer created by the ssd1306 driver library.
 * Every driver function (clear, draw, refresh) requires this handle
 * so it knows which display instance to operate on. */
ssd1306_handle_t ssd1306_dev = NULL;

/* oled_init()
 * -----------
 * Brings up the I2C bus and initializes the SSD1306 display.
 * Must be called once before any drawing functions are used.
 *
 * Step 1 — Configure I2C:
 *   Fill an i2c_config_t struct with our pin numbers, mode (master),
 *   pull-up settings, and clock speed, then apply it with i2c_param_config().
 *
 * Step 2 — Install the I2C driver:
 *   i2c_driver_install() registers the I2C peripheral with FreeRTOS so it
 *   can be used by tasks. The zeros at the end mean no RX/TX ring buffers
 *   (not needed in master mode) and no interrupt flags.
 *
 * Step 3 — Create the SSD1306 device:
 *   ssd1306_create() allocates the driver's internal state (including the
 *   128×64 pixel gram buffer) and stores a pointer to it in ssd1306_dev.
 *
 * Step 4 — Clear and flush:
 *   ssd1306_clear_screen() fills the in-memory gram buffer with 0x00 (all
 *   pixels off). ssd1306_refresh_gram() then sends that buffer over I2C to
 *   the physical display, ensuring we start with a blank screen.
 */
void oled_init(void)
{
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,                // ESP32 is the bus master
        .sda_io_num = I2C_MASTER_SDA_IO,        // Data line → GPIO 21
        .scl_io_num = I2C_MASTER_SCL_IO,        // Clock line → GPIO 22
        .sda_pullup_en = GPIO_PULLUP_ENABLE,    // Enable internal pull-ups
        .scl_pullup_en = GPIO_PULLUP_ENABLE,    // (required by the I2C spec)
        .master.clk_speed = I2C_MASTER_FREQ_HZ, // 400 kHz fast mode
    };

    /* Apply the configuration to the chosen I2C peripheral */
    ESP_ERROR_CHECK(i2c_param_config(I2C_MASTER_NUM, &i2c_conf));

    /* Register the I2C driver with the OS.
     * Arguments: port, mode, rx_buf_len, tx_buf_len, intr_flags */
    ESP_ERROR_CHECK(i2c_driver_install(I2C_MASTER_NUM, i2c_conf.mode, 0, 0, 0));

    ESP_LOGI(TAG, "I2C initialized");

    /* Ask the ssd1306 library to allocate its internal structures and
     * send the initialization command sequence to the display over I2C. */
    ssd1306_dev = ssd1306_create(I2C_MASTER_NUM, SSD1306_I2C_ADDRESS);
    if (ssd1306_dev == NULL)
    {
        /* If the display is not wired correctly or the address is wrong,
         * ssd1306_create returns NULL — log the error and bail out. */
        ESP_LOGE(TAG, "Failed to create SSD1306 device");
        return;
    }

    /* Fill the driver's internal pixel buffer with zeros (all pixels off) */
    ssd1306_clear_screen(ssd1306_dev, 0x00);

    /* Transfer the cleared buffer to the display via I2C so the screen
     * physically shows a blank image before we draw anything. */
    ssd1306_refresh_gram(ssd1306_dev);

    ESP_LOGI(TAG, "OLED initialized");
}

/* app_main()
 * ----------
 * Entry point called by the ESP-IDF runtime after the system boots.
 * Equivalent to main() in a standard C program.
 *
 * The rendering pipeline used here has three stages:
 *   1. ssd1306_clear_screen()  — reset the in-memory pixel buffer
 *   2. ssd1306_draw_string()   — write text pixels into the buffer
 *   3. ssd1306_refresh_gram()  — push the buffer to the physical display
 *
 * Nothing appears on screen until step 3 is called — the display only
 * updates when the full gram buffer is transferred over I2C.
 */
void app_main(void)
{
    ESP_LOGI(TAG, "Starting Hello World");

    /* Initialize I2C and the OLED display */
    oled_init();

    /* ssd1306_draw_string(handle, x, y, text, font_size, color)
     *
     *   handle    — the device instance returned by ssd1306_create()
     *   x = 20    — horizontal pixel offset from the left edge
     *   y = 28    — vertical pixel offset from the top edge
     *               (28 centers the text on a 64px-tall display;
     *                use y=10 if your display is only 32px tall)
     *   text      — the string to render, cast to const uint8_t *
     *   font_size — character height in pixels (12 = small built-in font)
     *   color = 1 — 1 turns pixels ON (white); 0 turns pixels OFF (black)
     *
     * This call only writes into the in-memory gram buffer —
     * nothing is sent to the display yet. */
    ssd1306_draw_string(ssd1306_dev, 20, 28, (const uint8_t *)"Hello World", 12, 1);

    /* Flush the gram buffer to the physical display over I2C.
     * This is the call that makes "Hello World" actually appear on screen. */
    ssd1306_refresh_gram(ssd1306_dev);

    ESP_LOGI(TAG, "Hello World displayed");

    /* Keep the FreeRTOS task alive indefinitely.
     * app_main() must never return — if it does, the task is deleted
     * and the ESP32 may behave unexpectedly.
     * vTaskDelay() yields the CPU to other tasks while waiting,
     * so this loop consumes virtually no processing time. */
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000)); // Yield for 1000 ms, then loop
    }
}