/*
 * Application 5 — Dual-core IPC pipeline
 * Theme: SPACE (AOCS sample → attitude update → downlink packet)
 */

#ifndef USE_WEBSERVER
#define USE_WEBSERVER 1
#endif

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"

#if USE_WEBSERVER
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_server.h"

#define WIFI_SSID "Wokwi-GUEST"
#define WIFI_PASS ""
#define HTTP_PORT 80
#endif

#define BUTTON_GPIO GPIO_NUM_18

#define CONFIG_LOG_DEFAULT_LEVEL_INFO 1
#define CONFIG_LOG_MAXIMUM_LEVEL  5

static const char *TAG = "sat_telemetry";

/* ---------- Themed Telemetry Structures ---------- */
typedef struct {
    uint32_t timestamp_ms;
    uint32_t sequence_id;
    float attitude_quaternion[4];
    int16_t magnetic_field_nt[3];
} aocs_sample_t;

/* ---------- IPC objects ---------- */
static QueueHandle_t      data_q; 
static EventGroupHandle_t evt_group;
static TaskHandle_t       responder_handle = NULL;

/* Event-group bit definitions */
#define EV_BIT_DATA_PRODUCED  (1 << 0)
#define EV_BIT_DATA_PROCESSED (1 << 1)

/* Per-task heartbeats */
static volatile uint32_t hb_prod, hb_cons, hb_coord, hb_resp;

/* Shared Observation Variables (atomic 32-bit reads) */
static volatile uint32_t last_processed_seq = 0;
static volatile int64_t isr_entry_time_us = 0;
static volatile uint64_t max_wakeup_latency_us = 0;

/* ---------- Producer task (Core 1) ---------- */
static void producer_task(void *arg)
{
    uint32_t seq = 0;
    for (;;) {
        aocs_sample_t sample = {
            .timestamp_ms = esp_log_timestamp(),
            .sequence_id = seq++,
            .attitude_quaternion = {1.0f, 0.0f, 0.0f, 0.0f},
            .magnetic_field_nt = {3120, -450, 1200}
        };

        // Back-pressure policy: block up to 5ms, then drop if completely saturated
        if (xQueueSend(data_q, &sample, pdMS_TO_TICKS(5)) != pdTRUE) {
            ESP_LOGW(TAG, "[AOCS-Prod] Telemetry Queue Saturation! Telemetry Dropped, ID: %lu", (unsigned long)sample.sequence_id);
        } else {
            xEventGroupSetBits(evt_group, EV_BIT_DATA_PRODUCED);
        }

        hb_prod++;
        vTaskDelay(pdMS_TO_TICKS(50));   /* 20 Hz telemetry sample rate */
    }
}

/* ---------- Consumer task (Core 1) ---------- */
static void consumer_task(void *arg)
{
    aocs_sample_t received_item;
    for (;;) {
        // Blocks automatically on the queue without spinning
        if (xQueueReceive(data_q, &received_item, portMAX_DELAY) == pdTRUE) {
            last_processed_seq = received_item.sequence_id;
            xEventGroupSetBits(evt_group, EV_BIT_DATA_PROCESSED);
            hb_cons++;
        }
    }
}

/* ---------- Coordinator task (Core 1) ---------- */
static void coordinator_task(void *arg)
{
    const EventBits_t wait_mask = EV_BIT_DATA_PRODUCED | EV_BIT_DATA_PROCESSED;
    for (;;) {
        EventBits_t got = xEventGroupWaitBits(evt_group, wait_mask,
                                              pdTRUE,   /* clear on exit */
                                              pdTRUE,   /* wait for ALL */
                                              portMAX_DELAY);
        if ((got & wait_mask) == wait_mask) {
            // Safety guard: only notify if the handle has been allocated by app_main
            if (responder_handle != NULL) {
                xTaskNotifyGive(responder_handle);
            }
            hb_coord++;
        }
    }
}

/* ---------- Responder task (Core 1) ---------- */
static void responder_task(void *arg)
{
    for (;;) {
        uint32_t n = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (n == 0) continue;
        
        // Calculate wake latency if it originated from the button ISR
        int64_t isr_time = isr_entry_time_us;
        if (isr_time > 0) {
            int64_t lat = esp_timer_get_time() - isr_time;
            if ((uint64_t)lat > max_wakeup_latency_us) {
                max_wakeup_latency_us = (uint64_t)lat;
            }
            isr_entry_time_us = 0; // Clear marker
        }

        ESP_LOGI(TAG, "[Downlink-Resp] Frame streaming initiated (burst count=%lu)", (unsigned long)n);
        hb_resp++;
    }
}

/* ---------- Button ISR — notify responder directly ---------- */
static volatile int64_t last_edge_us;
static void IRAM_ATTR button_isr(void *arg)
{
    int64_t now = esp_timer_get_time();
    if (now - last_edge_us < 200) return; // 200us Software debounce
    last_edge_us = now;
    isr_entry_time_us = now;

    // Safety guard to avoid null pointer crashes mid-boot
    if (responder_handle != NULL) {
        BaseType_t woken = pdFALSE;
        vTaskNotifyGiveFromISR(responder_handle, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

#if USE_WEBSERVER
/* ---------- Web server handlers (Core 0) ---------- */
static esp_err_t handle_state(httpd_req_t *req)
{
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
        "{\"q_depth\":%u,\"evt_bits\":%u,\"hb_prod\":%lu,\"hb_cons\":%lu,\"hb_coord\":%lu,\"hb_resp\":%lu,\"last_seq\":%lu,\"max_lat_us\":%llu}",
        (unsigned)uxQueueMessagesWaiting(data_q),
        (unsigned)xEventGroupGetBits(evt_group),
        (unsigned long)hb_prod, (unsigned long)hb_cons, (unsigned long)hb_coord, (unsigned long)hb_resp,
        (unsigned long)last_processed_seq,
        (unsigned long long)max_wakeup_latency_us);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

static esp_err_t handle_root(httpd_req_t *req)
{
    static const char html[] =
        "<!DOCTYPE html>"
        "<html lang=\"en\"><head><meta charset=\"utf-8\"><title>SAT-1 Mission Monitor</title>"
        "<style>"
        "body { font-family: system-ui, sans-serif; background: #0b0f19; color: #e2e8f0; padding: 2rem; }"
        "h1 { color: #38bdf8; border-bottom: 2px solid #0284c7; padding-bottom: 0.5rem; }"
        "table { width: 100%; border-collapse: collapse; margin-top: 1rem; }"
        "th, td { padding: 0.75rem; text-align: left; border-bottom: 1px solid #1e293b; }"
        "th { background: #1e293b; color: #38bdf8; }"
        ".val { font-family: monospace; font-size: 1.15em; color: #f8fafc; }"
        "</style></head><body>"
        "<h1>SAT-1 Spacecraft Control Telemetry Matrix</h1>"
        "<table>"
        "<tr><th>Metric Group</th><th>Value</th></tr>"
        "<tr><td>AOCS Telemetry Queue Depth</td><td class=\"val\" id=\"q_depth\">--</td></tr>"
        "<tr><td>Event Group Bitmask</td><td class=\"val\" id=\"evt_bits\">--</td></tr>"
        "<tr><td>Last Synced Frame ID</td><td class=\"val\" id=\"last_seq\">--</td></tr>"
        "<tr><td>Max Manual Interrupt Latency</td><td class=\"val\" id=\"max_lat\">-- us</td></tr>"
        "<tr><td>Task Heartbeats (P / C / CO / R)</td><td class=\"val\" id=\"hb\">--</td></tr>"
        "</table>"
        "<script>"
        "async function update(){"
        "  try {"
        "    const r = await fetch('/state');"
        "    const d = await r.json();"
        "    document.getElementById('q_depth').textContent = d.q_depth;"
        "    document.getElementById('evt_bits').textContent = '0x' + d.evt_bits.toString(16).toUpperCase();"
        "    document.getElementById('last_seq').textContent = d.last_seq;"
        "    document.getElementById('max_lat').textContent = d.max_lat_us + ' us';"
        "    document.getElementById('hb').textContent = `P:${d.hb_prod} | C:${d.hb_cons} | CO:${d.hb_coord} | R:${d.hb_resp}`;"
        "  } catch(e){}"
        "}"
        "setInterval(update, 1000);"
        "update();"
        "</script></body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static httpd_handle_t start_webserver(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = HTTP_PORT;
    cfg.core_id = 0; 
    cfg.stack_size = 8192;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) == ESP_OK) {
        httpd_uri_t root_uri = { .uri = "/", .method = HTTP_GET, .handler = handle_root };
        httpd_uri_t state_uri = { .uri = "/state", .method = HTTP_GET, .handler = handle_state };
        httpd_register_uri_handler(server, &root_uri);
        httpd_register_uri_handler(server, &state_uri);
        ESP_LOGI(TAG, "Web Telemetry Monitor up on port %d", HTTP_PORT);
    }
    return server;
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) esp_wifi_connect();
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        start_webserver();
    }
}

static void webmonitor_task(void *arg)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_cfg = {
        .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS, .threshold.authmode = WIFI_AUTH_OPEN },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
#else
static void serial_monitor_task(void *arg)
{
    for (;;) {
        UBaseType_t depth = uxQueueMessagesWaiting(data_q);
        EventBits_t bits  = xEventGroupGetBits(evt_group);
        ESP_LOGI(TAG, "[monitor] q_depth=%u  evt=0x%02x  hb: prod=%lu cons=%lu coord=%lu resp=%lu | Latency Max=%lluus",
                 (unsigned)depth, (unsigned)bits,
                 (unsigned long)hb_prod, (unsigned long)hb_cons,
                 (unsigned long)hb_coord, (unsigned long)hb_resp,
                 (unsigned long long)max_wakeup_latency_us);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#endif

/* ---------- app_main ---------- */
void app_main(void)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);
    ESP_LOGI(TAG, "==== App 5 [SPACE] starting — IPC pipeline ====");

    data_q = xQueueCreate(16, sizeof(aocs_sample_t));
    evt_group = xEventGroupCreate();

    /* Tasks on Core 1 (Kept in identical layout order to original scaffold to prevent bootloader panics) */
    xTaskCreatePinnedToCore(producer_task,    "prod",   4096, NULL,  8, NULL, APP_CPU_NUM);
    xTaskCreatePinnedToCore(consumer_task,    "cons",   4096, NULL,  8, NULL, APP_CPU_NUM);
    xTaskCreatePinnedToCore(coordinator_task, "coord",  4096, NULL,  9, NULL, APP_CPU_NUM);
    xTaskCreatePinnedToCore(responder_task,   "resp",   4096, NULL, 12, &responder_handle, APP_CPU_NUM);

    /* Observability plane on Core 0 */
#if USE_WEBSERVER
    xTaskCreatePinnedToCore(webmonitor_task,    "webmon",  8192, NULL, 4, NULL, PRO_CPU_NUM);
#else
    xTaskCreatePinnedToCore(serial_monitor_task, "monitor", 4096, NULL, 4, NULL, PRO_CPU_NUM);
#endif

    /* Button ISR Setup */
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&cfg);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BUTTON_GPIO, button_isr, NULL);
}
