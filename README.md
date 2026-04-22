# ESP32 Stock Ticker — ESP-IDF

Real-time stock ticker on an ILI9341 2.8" TFT (320×240).  
Data source: **Yahoo Finance** (no API key required).

## Features
- Scrolling colour-coded ticker tape (green = up, red = down)
- 6 stock rows cycling every 5 s with gold highlight
- Company short name shown on the active row
- Auto-refresh every 60 s
- Falls back to demo data if WiFi is unavailable

---

## Hardware

| ESP32 pin | ILI9341 pin |
|-----------|-------------|
| 3.3 V     | VCC         |
| GND       | GND         |
| GPIO 23   | SDI / MOSI  |
| GPIO 18   | SCK         |
| GPIO  5   | CS          |
| GPIO  4   | DC / RS     |
| GPIO 22   | RST         |
| GPIO 15   | LED (backlight) |

> If your display has the LED pin wired directly to 3.3 V, set `PIN_BL` to `-1` in `main/config.h`.

---

## Prerequisites

- **ESP-IDF v5.x** installed and activated (`idf.py` on your PATH)  
  Install guide: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/

---

## Quick start

```bash
# 1. Clone / copy this folder
cd stock_ticker

# 2. Edit main/config.h — set WiFi credentials and your stock symbols
#    WIFI_SSID, WIFI_PASSWORD, STOCK_SYMBOLS, NUM_STOCKS
#    Optional: leave WIFI_SSID/WIFI_PASSWORD empty to enter runtime setup via serial

# 3. Set target
idf.py set-target esp32s3

# 4. (Optional) open menuconfig to review settings
idf.py menuconfig

# 5. Build
idf.py build

# 6. Flash & monitor
idf.py -p /dev/ttyUSB0 flash monitor
# On macOS the port is usually /dev/cu.usbserial-*
# On Windows: COM3, COM4, etc.
```

---

## Project structure

```
stock_ticker/
├── CMakeLists.txt          # root cmake
├── sdkconfig.defaults      # pre-configured IDF options
└── main/
    ├── CMakeLists.txt      # component cmake
    ├── config.h            # ← ALL user settings live here
    ├── stock.h             # shared stock_t struct
    ├── main.c              # app_main, WiFi, FreeRTOS tasks
    ├── display.h / .c      # ILI9341 driver + all drawing code
    └── yahoo.h  / .c       # Yahoo Finance HTTP fetch + JSON parse
```

---

## Customising

### Runtime WiFi setup (SSID/password/hostname)
If `WIFI_SSID` is empty and no saved WiFi config exists in NVS, the firmware prompts over serial during boot for:
- SSID
- Password
- Hostname

The values are saved in NVS under `wifi_cfg` and reused on next boot.

Defaults are controlled in `main/config.h`:
- `WIFI_HOSTNAME_DEFAULT`
- `WIFI_RUNTIME_SETUP_ENABLE`

### Change stocks
Edit `STOCK_SYMBOLS` and `NUM_STOCKS` in `main/config.h`.  
Yahoo Finance symbols work for stocks (`AAPL`), ETFs (`SPY`), crypto (`BTC-USD`), and indices (`^GSPC`).

### Refresh rate
Change `FETCH_INTERVAL_MS` (default 60 000 ms).  
Yahoo rate-limits aggressive polling — keep it above 30 s.

### Scroll speed
Lower `TICKER_SCROLL_MS` for faster scrolling (default 25 ms per 2-pixel step).

### Cycle speed
Change `CYCLE_INTERVAL_MS` (default 5 000 ms).

---

## How the Yahoo Finance fetch works

```
GET https://query1.finance.yahoo.com/v8/finance/chart/AAPL?interval=1d&range=1d
```

The `meta` object at the top of the JSON response contains:
- `regularMarketPrice`
- `regularMarketChange`
- `regularMarketChangePercent`
- `shortName`

A `User-Agent` header is **required** — Yahoo returns 429 without it.  
TLS certificate verification is skipped (acceptable for a read-only display device).

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| White screen | Check wiring; verify `PIN_DC` and `PIN_RST` |
| `HTTP 429` in logs | Yahoo is rate-limiting — increase `FETCH_INTERVAL_MS` |
| `JSON parse failed` | Response may be truncated; increase the initial HTTP response buffer size in `main/yahoo.c` |
| WiFi won't connect | Double-check SSID/password in `main/config.h`; ensure 2.4 GHz network |
| Garbled display | Lower `SPI_CLOCK_HZ` to `20 * 1000 * 1000` in `main/config.h` |
