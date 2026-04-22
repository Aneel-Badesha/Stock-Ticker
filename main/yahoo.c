/*
 * yahoo.c
 * Fetches real-time quote data from the unofficial Yahoo Finance endpoint:
 *   GET https://query1.finance.yahoo.com/v8/finance/chart/<SYMBOL>?interval=1d&range=1d
 *
 * The "meta" object near the top of the JSON response contains:
 *   regularMarketPrice, regularMarketChange, regularMarketChangePercent, shortName
 *
 * Notes:
 *  - Yahoo blocks requests without a User-Agent header (returns 429).
 *  - TLS verification uses ESP-IDF's built-in certificate bundle.
 *  - The response can be 20–50 KB; we stream into a heap buffer.
 */

#include "yahoo.h"
#include "config.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_http_client.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "yahoo";

// ── HTTP response accumulator ─────────────────────────────────────────────────

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} resp_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    resp_buf_t *rb = (resp_buf_t *)evt->user_data;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (!evt->data || evt->data_len == 0) break;

        // Grow buffer if needed
        if (rb->len + evt->data_len + 1 > rb->cap) {
            size_t new_cap = rb->cap + evt->data_len + 4096;
            char *tmp = realloc(rb->buf, new_cap);
            if (!tmp) {
                ESP_LOGE(TAG, "OOM growing response buffer");
                break;
            }
            rb->buf = tmp;
            rb->cap = new_cap;
        }
        memcpy(rb->buf + rb->len, evt->data, evt->data_len);
        rb->len += evt->data_len;
        rb->buf[rb->len] = '\0';
        break;

    case HTTP_EVENT_ON_FINISH:
    case HTTP_EVENT_DISCONNECTED:
        break;

    default:
        break;
    }
    return ESP_OK;
}

// ── Public API ────────────────────────────────────────────────────────────────

bool yahoo_fetch(const char *symbol, stock_t *out)
{
    char url[128];
    snprintf(url, sizeof(url),
             "https://query1.finance.yahoo.com/v8/finance/chart/%s"
             "?interval=1d&range=1d",
             symbol);

    resp_buf_t rb = { .buf = malloc(8192), .len = 0, .cap = 8192 };
    if (!rb.buf) {
        ESP_LOGE(TAG, "Failed to allocate response buffer");
        return false;
    }
    rb.buf[0] = '\0';

    esp_http_client_config_t cfg = {
        .url                = url,
        .event_handler      = http_event_handler,
        .user_data          = &rb,
        .timeout_ms         = 10000,
        .transport_type     = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach  = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(rb.buf);
        return false;
    }

    // Yahoo requires a User-Agent or it returns 429 / empty body
    esp_http_client_set_header(client, "User-Agent",
        "Mozilla/5.0 (compatible; ESP32StockTicker/1.0)");

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    bool ok = false;

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[%s] HTTP error: %s", symbol, esp_err_to_name(err));
        goto done;
    }
    if (status == 429) {
        ESP_LOGW(TAG, "[%s] Rate-limited by Yahoo (429) — keeping old data", symbol);
        free(rb.buf);
        return false;   // caller keeps existing valid data
    }
    if (status != 200) {
        ESP_LOGE(TAG, "[%s] HTTP status %d", symbol, status);
        goto done;
    }

    // ── Parse JSON ────────────────────────────────────────────────────────────
    cJSON *root = cJSON_Parse(rb.buf);
    if (!root) {
        ESP_LOGE(TAG, "[%s] cJSON_Parse failed", symbol);
        goto done;
    }

    // Navigate: root -> chart -> result[0] -> meta
    cJSON *chart   = cJSON_GetObjectItemCaseSensitive(root, "chart");
    cJSON *results = cJSON_GetObjectItemCaseSensitive(chart, "result");
    cJSON *result0 = cJSON_GetArrayItem(results, 0);
    cJSON *meta    = cJSON_GetObjectItemCaseSensitive(result0, "meta");

    if (!meta) {
        ESP_LOGE(TAG, "[%s] meta object not found in JSON", symbol);
        cJSON_Delete(root);
        goto done;
    }

    cJSON *j_price  = cJSON_GetObjectItemCaseSensitive(meta, "regularMarketPrice");
    cJSON *j_change = cJSON_GetObjectItemCaseSensitive(meta, "regularMarketChange");
    cJSON *j_pct    = cJSON_GetObjectItemCaseSensitive(meta, "regularMarketChangePercent");
    cJSON *j_name   = cJSON_GetObjectItemCaseSensitive(meta, "shortName");

    if (!cJSON_IsNumber(j_price)) {
        ESP_LOGE(TAG, "[%s] regularMarketPrice missing or not a number", symbol);
        cJSON_Delete(root);
        goto done;
    }

    strncpy(out->symbol, symbol, sizeof(out->symbol) - 1);
    out->symbol[sizeof(out->symbol) - 1] = '\0';

    out->price      = (float)j_price->valuedouble;
    out->change     = cJSON_IsNumber(j_change) ? (float)j_change->valuedouble : 0.0f;
    out->change_pct = cJSON_IsNumber(j_pct)    ? (float)j_pct->valuedouble    : 0.0f;

    if (cJSON_IsString(j_name) && j_name->valuestring && strlen(j_name->valuestring) > 0) {
        strncpy(out->short_name, j_name->valuestring, sizeof(out->short_name) - 1);
        out->short_name[sizeof(out->short_name) - 1] = '\0';
    } else {
        strncpy(out->short_name, symbol, sizeof(out->short_name) - 1);
    }

    out->valid = (out->price > 0.0f);
    ok = out->valid;

    cJSON_Delete(root);

done:
    free(rb.buf);
    return ok;
}
