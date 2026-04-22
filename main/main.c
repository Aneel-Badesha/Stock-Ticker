#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "config.h"
#include "stock.h"
#include "display.h"
#include "yahoo.h"

static const char *TAG = "main";

static stock_t g_stocks[MAX_STOCKS];
static int g_num_stocks = NUM_STOCKS;
static int g_active_stock = 0;
static SemaphoreHandle_t g_mutex;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static EventGroupHandle_t s_wifi_events;
static int s_retry_count = 0;

typedef struct {
    char ssid[33];
    char password[65];
    char hostname[33];
} wifi_runtime_cfg_t;

static void trim_line(char *s)
{
    if (!s) {
        return;
    }
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[len - 1] = '\0';
        len--;
    }
}

static bool read_line_prompt(const char *prompt, char *out, size_t out_len)
{
    if (!prompt || !out || out_len == 0) {
        return false;
    }

    printf("%s", prompt);
    fflush(stdout);

    if (!fgets(out, (int)out_len, stdin)) {
        return false;
    }

    trim_line(out);
    return true;
}

static void wifi_cfg_apply_defaults(wifi_runtime_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->ssid, WIFI_SSID, sizeof(cfg->ssid) - 1);
    strncpy(cfg->password, WIFI_PASSWORD, sizeof(cfg->password) - 1);
    strncpy(cfg->hostname, WIFI_HOSTNAME_DEFAULT, sizeof(cfg->hostname) - 1);
}

static bool wifi_cfg_load_from_nvs(wifi_runtime_cfg_t *cfg)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi_cfg", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return false;
    }

    memset(cfg, 0, sizeof(*cfg));
    size_t len = sizeof(cfg->ssid);
    err = nvs_get_str(nvs, "ssid", cfg->ssid, &len);
    if (err != ESP_OK || cfg->ssid[0] == '\0') {
        nvs_close(nvs);
        return false;
    }

    len = sizeof(cfg->password);
    if (nvs_get_str(nvs, "pass", cfg->password, &len) != ESP_OK) {
        cfg->password[0] = '\0';
    }

    len = sizeof(cfg->hostname);
    if (nvs_get_str(nvs, "host", cfg->hostname, &len) != ESP_OK || cfg->hostname[0] == '\0') {
        strncpy(cfg->hostname, WIFI_HOSTNAME_DEFAULT, sizeof(cfg->hostname) - 1);
    }

    nvs_close(nvs);
    return true;
}

static bool wifi_cfg_save_to_nvs(const wifi_runtime_cfg_t *cfg)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi_cfg", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(wifi_cfg) failed: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_str(nvs, "ssid", cfg->ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "pass", cfg->password);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "host", cfg->hostname);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed saving WiFi cfg: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Saved WiFi runtime config to NVS");
    return true;
}

static bool wifi_cfg_prompt_serial(wifi_runtime_cfg_t *cfg)
{
    char buf[96];

    printf("\n\n=== WiFi Runtime Setup ===\n");
    printf("Provide WiFi credentials and hostname over serial monitor.\n");

    if (!read_line_prompt("SSID: ", cfg->ssid, sizeof(cfg->ssid))) {
        return false;
    }
    if (cfg->ssid[0] == '\0') {
        ESP_LOGW(TAG, "Empty SSID provided; runtime setup aborted");
        return false;
    }

    if (!read_line_prompt("Password (leave blank for open WiFi): ", cfg->password, sizeof(cfg->password))) {
        return false;
    }

    if (!read_line_prompt("Hostname (blank uses default): ", buf, sizeof(buf))) {
        return false;
    }
    if (buf[0] != '\0') {
        strncpy(cfg->hostname, buf, sizeof(cfg->hostname) - 1);
        cfg->hostname[sizeof(cfg->hostname) - 1] = '\0';
    } else {
        strncpy(cfg->hostname, WIFI_HOSTNAME_DEFAULT, sizeof(cfg->hostname) - 1);
    }

    return true;
}

static bool wifi_cfg_load_or_provision(wifi_runtime_cfg_t *cfg)
{
    if (wifi_cfg_load_from_nvs(cfg)) {
        ESP_LOGI(TAG, "Loaded WiFi runtime config from NVS (ssid=%s, host=%s)", cfg->ssid, cfg->hostname);
        return true;
    }

    wifi_cfg_apply_defaults(cfg);

    if (cfg->ssid[0] != '\0') {
        ESP_LOGI(TAG, "Using WiFi defaults from config.h (ssid=%s, host=%s)", cfg->ssid, cfg->hostname);
        return true;
    }

    ESP_LOGW(TAG, "No WiFi config found in NVS or defaults");
    if (!WIFI_RUNTIME_SETUP_ENABLE) {
        return false;
    }

    if (!wifi_cfg_prompt_serial(cfg)) {
        return false;
    }

    return wifi_cfg_save_to_nvs(cfg);
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGW(TAG, "WiFi retry %d/%d", s_retry_count, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_connect(const wifi_runtime_cfg_t *wifi_runtime)
{
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_netif_set_hostname(sta_netif, wifi_runtime->hostname));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t inst_any;
    esp_event_handler_instance_t inst_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, &inst_any));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, &inst_got_ip));

    wifi_config_t wifi_cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_OPEN,
        },
    };

    strncpy((char *)wifi_cfg.sta.ssid, wifi_runtime->ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, wifi_runtime->password, sizeof(wifi_cfg.sta.password) - 1);
    if (wifi_runtime->password[0] != '\0') {
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(15000));

    bool ok = (bits & WIFI_CONNECTED_BIT) != 0;
    ESP_LOGI(TAG, "WiFi %s (ssid=%s, host=%s)",
             ok ? "connected" : "FAILED",
             wifi_runtime->ssid,
             wifi_runtime->hostname);
    return ok;
}

static void load_demo_data(void)
{
    static const struct {
        const char *sym;
        const char *name;
        float p;
        float c;
        float pct;
    } demo[] = {
        { "AAPL", "Apple Inc.", 189.30f, 2.45f, 1.31f },
        { "MSFT", "Microsoft", 415.20f, -1.30f, -0.31f },
        { "GOOGL", "Alphabet", 172.50f, 3.10f, 1.83f },
        { "AMZN", "Amazon", 185.60f, -0.85f, -0.46f },
        { "TSLA", "Tesla", 235.40f, 8.75f, 3.86f },
        { "NVDA", "NVIDIA", 875.90f, 15.30f, 1.78f },
    };

    int n = sizeof(demo) / sizeof(demo[0]);
    if (n > MAX_STOCKS) {
        n = MAX_STOCKS;
    }

    for (int i = 0; i < n; i++) {
        strncpy(g_stocks[i].symbol, demo[i].sym, sizeof(g_stocks[i].symbol) - 1);
        g_stocks[i].symbol[sizeof(g_stocks[i].symbol) - 1] = '\0';
        strncpy(g_stocks[i].short_name, demo[i].name, sizeof(g_stocks[i].short_name) - 1);
        g_stocks[i].short_name[sizeof(g_stocks[i].short_name) - 1] = '\0';
        g_stocks[i].price = demo[i].p;
        g_stocks[i].change = demo[i].c;
        g_stocks[i].change_pct = demo[i].pct;
        g_stocks[i].valid = true;
    }

    g_num_stocks = n;
}

static void fetch_task(void *pv_param)
{
    (void)pv_param;
    const char *symbols[] = STOCK_SYMBOLS;

    while (1) {
        display_show_status("Refreshing...");

        for (int i = 0; i < NUM_STOCKS; i++) {
            stock_t tmp = {0};
            if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                tmp = g_stocks[i];
                xSemaphoreGive(g_mutex);
            }

            bool ok = yahoo_fetch(symbols[i], &tmp);

            if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                if (ok) {
                    g_stocks[i] = tmp;
                } else if (!g_stocks[i].valid) {
                    strncpy(g_stocks[i].symbol, symbols[i], sizeof(g_stocks[i].symbol) - 1);
                    g_stocks[i].symbol[sizeof(g_stocks[i].symbol) - 1] = '\0';
                }
                xSemaphoreGive(g_mutex);
            }

            vTaskDelay(pdMS_TO_TICKS(300));
        }

        display_clear_status();

        if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            display_draw_stock_list(g_stocks, g_num_stocks, g_active_stock);
            xSemaphoreGive(g_mutex);
        }

        vTaskDelay(pdMS_TO_TICKS(FETCH_INTERVAL_MS));
    }
}

static void display_task(void *pv_param)
{
    (void)pv_param;
    TickType_t last_cycle = xTaskGetTickCount();
    TickType_t last_scroll = xTaskGetTickCount();

    while (1) {
        TickType_t now = xTaskGetTickCount();

        if ((now - last_scroll) * portTICK_PERIOD_MS >= TICKER_SCROLL_MS) {
            last_scroll = now;
            if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                display_scroll_ticker(g_stocks, g_num_stocks);
                xSemaphoreGive(g_mutex);
            }
        }

        if ((now - last_cycle) * portTICK_PERIOD_MS >= CYCLE_INTERVAL_MS) {
            last_cycle = now;
            if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                g_active_stock = (g_active_stock + 1) % g_num_stocks;
                display_draw_stock_list(g_stocks, g_num_stocks, g_active_stock);
                xSemaphoreGive(g_mutex);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    display_init();
    display_draw_boot_screen();
    display_boot_msg("Preparing WiFi config...", false);

    wifi_runtime_cfg_t wifi_runtime;
    bool have_wifi_cfg = wifi_cfg_load_or_provision(&wifi_runtime);

    g_mutex = xSemaphoreCreateMutex();

    const char *symbols[] = STOCK_SYMBOLS;
    for (int i = 0; i < NUM_STOCKS; i++) {
        strncpy(g_stocks[i].symbol, symbols[i], sizeof(g_stocks[i].symbol) - 1);
        g_stocks[i].symbol[sizeof(g_stocks[i].symbol) - 1] = '\0';
        g_stocks[i].valid = false;
    }

    bool wifi_ok = false;
    if (have_wifi_cfg) {
        display_boot_msg("Connecting to WiFi...", false);
        wifi_ok = wifi_connect(&wifi_runtime);
    }

    if (wifi_ok) {
        display_boot_msg("WiFi connected!", false);
        vTaskDelay(pdMS_TO_TICKS(800));
    } else {
        display_boot_msg("No WiFi - loading demo data", true);
        load_demo_data();
        vTaskDelay(pdMS_TO_TICKS(1500));
    }

    display_draw_main_screen();
    display_draw_stock_list(g_stocks, g_num_stocks, g_active_stock);

    xTaskCreatePinnedToCore(display_task, "display", 4096, NULL, 5, NULL, 1);

    if (wifi_ok) {
        xTaskCreatePinnedToCore(fetch_task, "fetch", 8192, NULL, 3, NULL, 0);
    }
}
