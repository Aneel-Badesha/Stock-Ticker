/*
 * display.c
 * Bit-banging-free ILI9341 driver using ESP-IDF spi_master.
 * All drawing uses direct register writes — no third-party display library.
 *
 * Coordinate system: landscape 320 × 240 (MADCTL = 0x28).
 *
 * Font: built-in 8×8 pixel glyphs from the public-domain "font8x8_basic" table
 * embedded at the bottom of this file.  For nicer text, swap in lvgl or
 * the Adafruit GFX bitmap fonts compiled to plain C arrays.
 */

#include "display.h"
#include "config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "display";

// ── SPI / ILI9341 internals ───────────────────────────────────────────────────

static spi_device_handle_t s_spi = NULL;
static SemaphoreHandle_t s_display_mutex = NULL;

static inline void display_lock(void)
{
    if (s_display_mutex) {
        xSemaphoreTake(s_display_mutex, portMAX_DELAY);
    }
}

static inline void display_unlock(void)
{
    if (s_display_mutex) {
        xSemaphoreGive(s_display_mutex);
    }
}

static inline void dc_cmd(void)  { gpio_set_level(PIN_DC, 0); }
static inline void dc_data(void) { gpio_set_level(PIN_DC, 1); }

static inline void bl_set(bool on)
{
#if PIN_BL >= 0
    gpio_set_level(PIN_BL, on ? PIN_BL_ON_LEVEL : !PIN_BL_ON_LEVEL);
#else
    (void)on;
#endif
}

static void spi_write_byte(uint8_t b)
{
    spi_transaction_t t = {
        .length    = 8,
        .tx_buffer = &b,
    };
    spi_device_polling_transmit(s_spi, &t);
}

static void spi_write_buf(const uint8_t *buf, size_t len)
{
    if (len == 0) return;
    spi_transaction_t t = {
        .length    = len * 8,
        .tx_buffer = buf,
    };
    spi_device_polling_transmit(s_spi, &t);
}

static void ili_cmd(uint8_t cmd)
{
    dc_cmd();
    spi_write_byte(cmd);
}

static void ili_data(uint8_t d)
{
    dc_data();
    spi_write_byte(d);
}

// ── ILI9341 initialisation sequence ──────────────────────────────────────────

static void ili9341_init(void)
{
    // Hardware reset
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));

    ili_cmd(0x01); vTaskDelay(pdMS_TO_TICKS(5));   // SW reset
    ili_cmd(0x28);                                  // display off

    ili_cmd(0xCF);
    ili_data(0x00); ili_data(0xC1); ili_data(0x30);

    ili_cmd(0xED);
    ili_data(0x64); ili_data(0x03); ili_data(0x12); ili_data(0x81);

    ili_cmd(0xE8);
    ili_data(0x85); ili_data(0x00); ili_data(0x78);

    ili_cmd(0xCB);
    ili_data(0x39); ili_data(0x2C); ili_data(0x00); ili_data(0x34); ili_data(0x02);

    ili_cmd(0xF7); ili_data(0x20);

    ili_cmd(0xEA); ili_data(0x00); ili_data(0x00);

    ili_cmd(0xC0); ili_data(0x23);          // Power control — VRH[5:0]
    ili_cmd(0xC1); ili_data(0x10);          // Power control — SAP, BT[3:0]
    ili_cmd(0xC5); ili_data(0x3E); ili_data(0x28);  // VCOM control
    ili_cmd(0xC7); ili_data(0x86);          // VCOM control 2

    // Landscape: MX=1, MV=1  →  MADCTL 0x28
    ili_cmd(0x36); ili_data(0x28);

    ili_cmd(0x3A); ili_data(0x55);          // 16-bit colour (RGB565)

    ili_cmd(0xB1); ili_data(0x00); ili_data(0x18);  // frame rate ~70 Hz

    ili_cmd(0xB6);
    ili_data(0x08); ili_data(0x82); ili_data(0x27);

    ili_cmd(0xF2); ili_data(0x00);          // 3-gamma off
    ili_cmd(0x26); ili_data(0x01);          // gamma curve 1

    ili_cmd(0xE0);  // positive gamma
    ili_data(0x0F); ili_data(0x31); ili_data(0x2B); ili_data(0x0C);
    ili_data(0x0E); ili_data(0x08); ili_data(0x4E); ili_data(0xF1);
    ili_data(0x37); ili_data(0x07); ili_data(0x10); ili_data(0x03);
    ili_data(0x0E); ili_data(0x09); ili_data(0x00);

    ili_cmd(0xE1);  // negative gamma
    ili_data(0x00); ili_data(0x0E); ili_data(0x14); ili_data(0x03);
    ili_data(0x11); ili_data(0x07); ili_data(0x31); ili_data(0xC1);
    ili_data(0x48); ili_data(0x08); ili_data(0x0F); ili_data(0x0C);
    ili_data(0x31); ili_data(0x36); ili_data(0x0F);

    ili_cmd(0x11); vTaskDelay(pdMS_TO_TICKS(120));  // sleep out
    ili_cmd(0x29);                                   // display on

#if PIN_BL >= 0
    bl_set(false);
    vTaskDelay(pdMS_TO_TICKS(10));
    bl_set(true);
#endif
}

// ── Pixel / rect primitives ───────────────────────────────────────────────────

static void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    ili_cmd(0x2A);
    dc_data();
    uint8_t ca[4] = { x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF };
    spi_write_buf(ca, 4);

    ili_cmd(0x2B);
    dc_data();
    uint8_t ra[4] = { y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF };
    spi_write_buf(ra, 4);

    ili_cmd(0x2C);
    dc_data();
}

static void fill_rect(int x, int y, int w, int h, uint16_t col)
{
    if (w <= 0 || h <= 0) return;
    set_window(x, y, x + w - 1, y + h - 1);

    uint8_t hi = col >> 8, lo = col & 0xFF;
    // Send in 64-byte bursts for speed
    uint8_t buf[128];
    for (int i = 0; i < 64; i++) { buf[i*2] = hi; buf[i*2+1] = lo; }
    int pixels = w * h;
    while (pixels >= 64) {
        spi_write_buf(buf, 128);
        pixels -= 64;
    }
    for (int i = 0; i < pixels; i++) {
        spi_write_byte(hi);
        spi_write_byte(lo);
    }
}

static void draw_hline(int x, int y, int w, uint16_t col)
{
    fill_rect(x, y, w, 1, col);
}

// ── Filled triangle (for arrows) ─────────────────────────────────────────────

static void fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t col)
{
    // Sort by y
    if (y0 > y1) { int t; t=y0;y0=y1;y1=t; t=x0;x0=x1;x1=t; }
    if (y1 > y2) { int t; t=y1;y1=y2;y2=t; t=x1;x1=x2;x2=t; }
    if (y0 > y1) { int t; t=y0;y0=y1;y1=t; t=x0;x0=x1;x1=t; }

    for (int y = y0; y <= y2; y++) {
        float t1 = (y2 == y0) ? 1.0f : (float)(y - y0) / (y2 - y0);
        int xa = x0 + (int)((x2 - x0) * t1);
        int xb;
        if (y <= y1) {
            float t2 = (y1 == y0) ? 1.0f : (float)(y - y0) / (y1 - y0);
            xb = x0 + (int)((x1 - x0) * t2);
        } else {
            float t2 = (y2 == y1) ? 1.0f : (float)(y - y1) / (y2 - y1);
            xb = x1 + (int)((x2 - x1) * t2);
        }
        if (xa > xb) { int t = xa; xa = xb; xb = t; }
        draw_hline(xa, y, xb - xa + 1, col);
    }
}

// ── Embedded 8×8 font (public domain) ────────────────────────────────────────
// Subset: ASCII 0x20–0x7E  (printable characters)

static const uint8_t FONT8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 0x20 space
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, // !
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, // "
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, // #
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, // $
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, // %
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, // &
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, // '
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, // (
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, // )
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // *
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, // +
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, // ,
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, // -
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, // .
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, // /
    {0x1E,0x33,0x3B,0x37,0x33,0x33,0x1E,0x00}, // 0
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, // 1
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, // 2
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, // 3
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, // 4
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, // 5
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, // 6
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, // 7
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, // 8
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, // 9
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, // :
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, // ;
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, // <
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, // =
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, // >
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, // ?
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, // @
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, // A
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, // B
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, // C
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, // D
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, // E
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, // F
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, // G
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, // H
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // I
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, // J
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, // K
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, // L
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, // M
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, // N
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, // O
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, // P
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, // Q
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, // R
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, // S
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // T
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, // U
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, // V
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, // W
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, // X
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, // Y
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, // Z
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, // [
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, // backslash
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, // ]
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, // ^
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, // _
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, // `
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, // a
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, // b
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, // c
    {0x38,0x30,0x30,0x3e,0x33,0x33,0x6E,0x00}, // d
    {0x00,0x00,0x1E,0x33,0x3f,0x03,0x1E,0x00}, // e
    {0x1C,0x36,0x06,0x0f,0x06,0x06,0x0F,0x00}, // f
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, // g
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, // h
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, // i
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, // j
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, // k
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // l
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, // m
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, // n
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, // o
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, // p
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, // q
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, // r
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, // s
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, // t
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, // u
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, // v
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, // w
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, // x
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, // y
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, // z
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, // {
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, // |
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, // }
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}, // ~
};

// ── Text drawing ──────────────────────────────────────────────────────────────

// Scale: 1 = 8×8 px,  2 = 16×16 px,  etc.
static void draw_char(int x, int y, char c, uint16_t fg, uint16_t bg, int scale)
{
    if (c < 0x20 || c > 0x7E) c = '?';
    const uint8_t *bm = FONT8[(uint8_t)c - 0x20];
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            uint16_t col16 = (bm[row] & (1 << (7 - col))) ? fg : bg;
            fill_rect(x + col * scale, y + row * scale, scale, scale, col16);
        }
    }
}

static void draw_text(int x, int y, const char *str, uint16_t fg, uint16_t bg, int scale)
{
    while (*str) {
        draw_char(x, y, *str++, fg, bg, scale);
        x += 8 * scale;
        if (x > LCD_W) break;
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void display_init(void)
{
    s_display_mutex = xSemaphoreCreateMutex();
    if (!s_display_mutex) {
        ESP_LOGE(TAG, "Failed to create display mutex");
        return;
    }

    // Configure GPIO
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_DC) | (1ULL << PIN_RST)
#if PIN_BL >= 0
                      | (1ULL << PIN_BL)
#endif
                      ,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    // Init SPI bus
    spi_bus_config_t buscfg = {
        .mosi_io_num   = PIN_MOSI,
        .miso_io_num   = PIN_MISO,
        .sclk_io_num   = PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * 2 * 16,  // enough for 16 rows at a time
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = SPI_CLOCK_HZ,
        .mode           = 0,
        .spics_io_num   = PIN_CS,
        .queue_size     = 7,
        .pre_cb         = NULL,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi));

    ili9341_init();
    ESP_LOGI(TAG, "ILI9341 initialised (%d×%d)", LCD_W, LCD_H);
}

void display_draw_boot_screen(void)
{
    display_lock();
    fill_rect(0, 0, LCD_W, LCD_H, COL_BG);

    // Header panel
    fill_rect(0, 0, LCD_W, 50, COL_PANEL);
    draw_hline(0, 50, LCD_W, COL_GOLD);

    // Title  — scale-2 font  (16×16 px chars)
    draw_text(14, 10, "ESP32  STOCK  TICKER", COL_GOLD, COL_PANEL, 2);

    draw_text(52, 60, "Powered by Yahoo Finance", COL_GRAY, COL_BG, 1);
    draw_text(76, 72, "No API key required", COL_GRAY, COL_BG, 1);

    draw_hline(20, 84, 280, COL_BORDER);
    display_unlock();
}

void display_boot_msg(const char *msg, bool error)
{
    display_lock();
    fill_rect(0, 130, LCD_W, 30, COL_BG);
    uint16_t col = error ? COL_RED : COL_GRAY;
    draw_text(12, 138, msg, col, COL_BG, 1);
    display_unlock();
}

void display_draw_main_screen(void)
{
    display_lock();
    fill_rect(0, 0, LCD_W, LCD_H, COL_BG);

    // Header
    fill_rect(0, 0, LCD_W, HEADER_H, COL_PANEL);
    draw_hline(0, HEADER_H, LCD_W, COL_BORDER);
    draw_text(6, 8, "MARKET WATCH", COL_GOLD, COL_PANEL, 1);
    draw_text(168, 8, "Yahoo Finance", COL_GRAY, COL_PANEL, 1);

    // Ticker bar
    fill_rect(0, TICKER_BAR_Y, LCD_W, TICKER_BAR_H, COL_TICKER_BG);
    draw_hline(0, TICKER_BAR_Y, LCD_W, COL_BORDER);
    display_unlock();
}

void display_show_status(const char *msg)
{
    display_lock();
    fill_rect(168, 1, 148, HEADER_H - 2, COL_PANEL);
    draw_text(172, 8, msg, COL_GOLD, COL_PANEL, 1);
    display_unlock();
}

void display_clear_status(void)
{
    display_lock();
    fill_rect(168, 1, 148, HEADER_H - 2, COL_PANEL);
    draw_text(168, 8, "Yahoo Finance", COL_GRAY, COL_PANEL, 1);
    display_unlock();
}

// ── Stock list ────────────────────────────────────────────────────────────────

void display_draw_stock_list(const stock_t *stocks, int count, int active_idx)
{
    display_lock();
    for (int i = 0; i < count; i++) {
        int  y        = ROWS_START_Y + i * ROW_H;
        bool is_active = (i == active_idx);

        uint16_t row_bg = is_active ? COL_PANEL : COL_BG;
        fill_rect(0, y, LCD_W, ROW_H - 1, row_bg);

        if (is_active) {
            fill_rect(0, y, 3, ROW_H - 1, COL_GOLD);
        }
        draw_hline(0, y + ROW_H - 1, LCD_W, COL_BORDER);

        if (!stocks[i].valid) {
            draw_text(8, y + 10, stocks[i].symbol, COL_GRAY, row_bg, 1);
            draw_text(72, y + 10, "--", COL_GRAY, row_bg, 1);
            continue;
        }

        bool     up     = stocks[i].change >= 0.0f;
        uint16_t chg_col = up ? COL_GREEN : COL_RED;
        uint16_t sym_col = is_active ? COL_WHITE : COL_GRAY;

        // Symbol
        draw_text(8, y + 10, stocks[i].symbol, sym_col, row_bg, 1);

        // Company short name on active row
        if (is_active) {
            char name_buf[14] = { 0 };
            strncpy(name_buf, stocks[i].short_name, 13);
            draw_text(70, y + 10, name_buf, COL_GRAY, row_bg, 1);
        }

        // Price
        char price_buf[16];
        snprintf(price_buf, sizeof(price_buf), "$%.2f", stocks[i].price);
        uint16_t price_col = is_active ? COL_WHITE : COL_GRAY;
        draw_text(182, y + 10, price_buf, price_col, row_bg, 1);

        // Change %
        char pct_buf[12];
        snprintf(pct_buf, sizeof(pct_buf), "%s%.2f%%",
                 up ? "+" : "", stocks[i].change_pct);
        draw_text(255, y + 10, pct_buf, chg_col, row_bg, 1);

        // Arrow
        int ax = 309, ay = y + 15;
        if (up) {
            fill_triangle(ax, ay - 6, ax - 4, ay + 2, ax + 4, ay + 2, COL_GREEN);
        } else {
            fill_triangle(ax, ay + 4, ax - 4, ay - 4, ax + 4, ay - 4, COL_RED);
        }
    }
    display_unlock();
}

// ── Ticker tape ───────────────────────────────────────────────────────────────

// We build one flat string and track an x offset.
static char   s_ticker_str[512];
static int    s_ticker_x   = LCD_W;
static int    s_ticker_len = 0;  // pixel width estimate

static void rebuild_ticker(const stock_t *stocks, int count)
{
    s_ticker_str[0] = '\0';
    for (int i = 0; i < count; i++) {
        if (!stocks[i].valid) continue;
        char seg[48];
        bool up = stocks[i].change_pct >= 0.0f;
        snprintf(seg, sizeof(seg), "    %s $%.2f  %s%.2f%%   *",
                 stocks[i].symbol,
                 stocks[i].price,
                 up ? "+" : "",
                 stocks[i].change_pct);
        strncat(s_ticker_str, seg, sizeof(s_ticker_str) - strlen(s_ticker_str) - 1);
    }
    s_ticker_len = strlen(s_ticker_str) * 8;  // 8 px per char at scale-1
    s_ticker_x   = LCD_W;
}

void display_scroll_ticker(const stock_t *stocks, int count)
{
    display_lock();
    if (s_ticker_len == 0) {
        rebuild_ticker(stocks, count);
        display_unlock();
        return;
    }

    const int BAR_Y  = TICKER_BAR_Y + 1;
    const int BAR_H  = TICKER_BAR_H - 2;
    const int TEXT_Y = BAR_Y + (BAR_H - 8) / 2;

    fill_rect(0, BAR_Y, LCD_W, BAR_H, COL_TICKER_BG);

    // Draw color-coded segments
    int x = s_ticker_x;
    for (int i = 0; i < count; i++) {
        if (!stocks[i].valid) continue;
        bool up = stocks[i].change_pct >= 0.0f;

        // Symbol — white
        char sym[16];
        snprintf(sym, sizeof(sym), "    %s", stocks[i].symbol);
        draw_text(x, TEXT_Y, sym, COL_WHITE, COL_TICKER_BG, 1);
        x += strlen(sym) * 8;

        // Price + change — green / red
        char val[32];
        snprintf(val, sizeof(val), " $%.2f  %s%.2f%%   *",
                 stocks[i].price,
                 up ? "+" : "",
                 stocks[i].change_pct);
        draw_text(x, TEXT_Y, val, up ? COL_GREEN : COL_RED, COL_TICKER_BG, 1);
        x += strlen(val) * 8;
    }

    s_ticker_x -= 2;
    if (s_ticker_x < -s_ticker_len) {
        s_ticker_x = LCD_W;
    }
    display_unlock();
}
