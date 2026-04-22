#pragma once

// WiFi
#define WIFI_SSID       ""
#define WIFI_PASSWORD   ""
#define WIFI_MAX_RETRY  10
#define WIFI_HOSTNAME_DEFAULT "stock-ticker-s3"
// If SSID/PASSWORD and NVS config are empty, prompt via serial monitor and save to NVS.
#define WIFI_RUNTIME_SETUP_ENABLE 1

// Stocks
// Any valid Yahoo Finance symbol: stocks, ETFs, crypto (BTC-USD), indices (^GSPC)
#define MAX_STOCKS      8
#define STOCK_SYMBOLS   { "AAPL", "MSFT", "GOOGL", "AMZN", "TSLA", "NVDA" }
#define NUM_STOCKS      6

// Timing (milliseconds)
#define FETCH_INTERVAL_MS   60000
#define CYCLE_INTERVAL_MS    5000
#define TICKER_SCROLL_MS       25

// ILI9341 SPI pins (ESP32-S3)
#define PIN_MOSI    23
#define PIN_CLK     18
#define PIN_CS       5
#define PIN_DC       4
#define PIN_RST     22
#define PIN_BL      15   // backlight, set to -1 if wired directly to 3.3 V

#define SPI_CLOCK_HZ    (40 * 1000 * 1000)

// Display geometry
#define LCD_W           320
#define LCD_H           240

// Layout constants
#define HEADER_H         26
#define TICKER_BAR_Y    215
#define TICKER_BAR_H     25
#define ROW_H            31
#define ROWS_START_Y     27

// RGB565 colour palette
#define COL_BG          0x0841
#define COL_PANEL       0x10A3
#define COL_BORDER      0x2945
#define COL_GREEN       0x07E0
#define COL_RED         0xF800
#define COL_WHITE       0xFFFF
#define COL_GRAY        0x8C71
#define COL_GOLD        0xFEA0
#define COL_TICKER_BG   0x0020
