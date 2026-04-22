#pragma once

// WiFi
#define WIFI_SSID       "BadeshaHome"
#define WIFI_PASSWORD   "Canucks@2011"
#define WIFI_MAX_RETRY  10
#define WIFI_HOSTNAME_DEFAULT "stock-ticker-s3"
// If SSID/PASSWORD and NVS config are empty, prompt via serial monitor and save to NVS.
#define WIFI_RUNTIME_SETUP_ENABLE 0

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
// These defaults are valid ESP32-S3 GPIOs; change to match your wiring.
#define PIN_MOSI    11
#define PIN_CLK     12
#define PIN_CS      10
#define PIN_DC       9
#define PIN_RST      8
#define PIN_BL       7   // backlight, set to -1 if wired directly to 3.3 V

#define SPI_CLOCK_HZ    (20 * 1000 * 1000)

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
