#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "mdns.h"
#include "led_strip.h"
#include "led_strip_rmt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "wakeup_config.h"

#define LED_COUNT 1
#define RMT_RESOLUTION_HZ 10000000
#define EASING_SCALE 65535U
#define WIFI_CONNECTED_BIT BIT0
#define MAX_RAMP_DURATION_SECONDS 21600U
#define DEVICE_HOSTNAME "wakeup-light"

#if WAKEUP_RAMP_DURATION_SECONDS < 2
#error WAKEUP_RAMP_DURATION_SECONDS must be at least 2
#endif

#if WAKEUP_HOUR < 0 || WAKEUP_HOUR > 23
#error WAKEUP_HOUR must be between 0 and 23
#endif

#if WAKEUP_MINUTE < 0 || WAKEUP_MINUTE > 59
#error WAKEUP_MINUTE must be between 0 and 59
#endif

static const char *TAG = "wakeup_light";
static EventGroupHandle_t wifi_event_group;
static SemaphoreHandle_t schedule_mutex;
static httpd_handle_t web_server;
static bool clock_synchronized;
static uint32_t schedule_generation;
static time_t next_ramp_start_time;

typedef struct {
    uint8_t wake_hour;
    uint8_t wake_minute;
    uint32_t ramp_duration_seconds;
} schedule_settings_t;

typedef enum {
    APP_STATE_SYNCING_TIME,
    APP_STATE_WAITING,
    APP_STATE_RAMPING,
    APP_STATE_COMPLETE,
} app_state_t;

static schedule_settings_t schedule_settings = {
    .wake_hour = WAKEUP_HOUR,
    .wake_minute = WAKEUP_MINUTE,
    .ramp_duration_seconds = WAKEUP_RAMP_DURATION_SECONDS,
};
static app_state_t app_state = APP_STATE_SYNCING_TIME;

static void set_app_state(app_state_t state)
{
    xSemaphoreTake(schedule_mutex, portMAX_DELAY);
    app_state = state;
    xSemaphoreGive(schedule_mutex);
}

static const char *app_state_name(app_state_t state)
{
    switch (state) {
    case APP_STATE_SYNCING_TIME:
        return "Synchronizing time";
    case APP_STATE_WAITING:
        return "Waiting for wake time";
    case APP_STATE_RAMPING:
        return "Ramp in progress";
    case APP_STATE_COMPLETE:
        return "Ramp complete; reset to re-arm";
    default:
        return "Unknown";
    }
}

static void load_schedule_settings(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t result = nvs_open("wakeup", NVS_READONLY, &nvs_handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return;
    }
    ESP_ERROR_CHECK(result);

    nvs_get_u8(nvs_handle, "wake_hour", &schedule_settings.wake_hour);
    nvs_get_u8(nvs_handle, "wake_minute", &schedule_settings.wake_minute);
    nvs_get_u32(nvs_handle, "ramp_seconds", &schedule_settings.ramp_duration_seconds);
    nvs_close(nvs_handle);

    if (schedule_settings.wake_hour > 23 || schedule_settings.wake_minute > 59 ||
        schedule_settings.ramp_duration_seconds < 2 ||
        schedule_settings.ramp_duration_seconds > MAX_RAMP_DURATION_SECONDS) {
        ESP_LOGW(TAG, "Stored schedule is invalid; using build-time defaults");
        schedule_settings = (schedule_settings_t) {
            .wake_hour = WAKEUP_HOUR,
            .wake_minute = WAKEUP_MINUTE,
            .ramp_duration_seconds = WAKEUP_RAMP_DURATION_SECONDS,
        };
    }
}

static void get_schedule_snapshot(schedule_settings_t *settings, uint32_t *generation)
{
    xSemaphoreTake(schedule_mutex, portMAX_DELAY);
    *settings = schedule_settings;
    if (generation != NULL) {
        *generation = schedule_generation;
    }
    xSemaphoreGive(schedule_mutex);
}

static esp_err_t save_schedule_settings(const schedule_settings_t *settings)
{
    nvs_handle_t nvs_handle;
    esp_err_t result = nvs_open("wakeup", NVS_READWRITE, &nvs_handle);
    if (result != ESP_OK) {
        return result;
    }

    result = nvs_set_u8(nvs_handle, "wake_hour", settings->wake_hour);
    if (result == ESP_OK) {
        result = nvs_set_u8(nvs_handle, "wake_minute", settings->wake_minute);
    }
    if (result == ESP_OK) {
        result = nvs_set_u32(nvs_handle, "ramp_seconds", settings->ramp_duration_seconds);
    }
    if (result == ESP_OK) {
        result = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
    if (result != ESP_OK) {
        return result;
    }

    xSemaphoreTake(schedule_mutex, portMAX_DELAY);
    schedule_settings = *settings;
    schedule_generation++;
    xSemaphoreGive(schedule_mutex);
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "Wi-Fi disconnected; retrying");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        ESP_LOGI(TAG, "Connected; IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void start_wifi(void)
{
    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES || nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_result);

    schedule_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(schedule_mutex == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    load_schedule_settings();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *wifi_netif = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_netif_set_hostname(wifi_netif, DEVICE_HOSTNAME));

    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_wifi_init(&(wifi_init_config_t)WIFI_INIT_CONFIG_DEFAULT()));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {0};
    memcpy(wifi_config.sta.ssid, WIFI_SSID, strlen(WIFI_SSID));
    memcpy(wifi_config.sta.password, WIFI_PASSWORD, strlen(WIFI_PASSWORD));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static bool wait_for_wifi(void)
{
    while ((xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                                pdMS_TO_TICKS(60000)) & WIFI_CONNECTED_BIT) == 0) {
        ESP_LOGW(TAG, "Waiting for Wi-Fi connection");
    }
    return true;
}

static bool synchronize_clock(void)
{
    const esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp_config));

    bool synchronized = false;
    for (int attempt = 0; attempt < 30; attempt++) {
        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(1000)) == ESP_OK) {
            synchronized = true;
            break;
        }
    }
    esp_netif_sntp_deinit();
    return synchronized;
}

static uint8_t interpolate_smooth(uint8_t from, uint8_t to, uint32_t elapsed_ms, uint32_t phase_ms)
{
    const uint64_t progress = (uint64_t)elapsed_ms * EASING_SCALE / phase_ms;
    const uint64_t progress_squared = progress * progress / EASING_SCALE;
    const uint64_t eased_progress = progress_squared * (3 * EASING_SCALE - 2 * progress) / EASING_SCALE;
    const int32_t difference = (int32_t)to - (int32_t)from;
    return (uint8_t)((int32_t)from + ((int64_t)difference * (int64_t)eased_progress) / EASING_SCALE);
}

static void run_ramp(led_strip_handle_t led_strip, uint32_t ramp_duration_seconds)
{
    const uint32_t ramp_duration_ms = ramp_duration_seconds * 1000U;
    const uint32_t first_phase_ms = ramp_duration_ms / 2;
    const uint32_t second_phase_ms = ramp_duration_ms - first_phase_ms;
    const int64_t start_time_us = esp_timer_get_time();

    while (true) {
        const uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - start_time_us) / 1000);
        uint8_t red;
        uint8_t green;
        uint8_t blue;

        if (elapsed_ms >= ramp_duration_ms) {
            red = FINAL_RED;
            green = FINAL_GREEN;
            blue = FINAL_BLUE;
        } else if (elapsed_ms < first_phase_ms) {
            red = interpolate_smooth(0, 255, elapsed_ms, first_phase_ms);
            green = 0;
            blue = 0;
        } else {
            const uint32_t phase_elapsed_ms = elapsed_ms - first_phase_ms;
            red = 255;
            green = interpolate_smooth(0, FINAL_GREEN, phase_elapsed_ms, second_phase_ms);
            blue = interpolate_smooth(0, FINAL_BLUE, phase_elapsed_ms, second_phase_ms);
        }

        ESP_ERROR_CHECK(led_strip_set_pixel(led_strip, 0, red, green, blue));
        ESP_ERROR_CHECK(led_strip_refresh(led_strip));

        if (elapsed_ms >= ramp_duration_ms) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static time_t get_next_ramp_start(time_t now, const schedule_settings_t *settings)
{
    struct tm wake_time_local;
    localtime_r(&now, &wake_time_local);
    wake_time_local.tm_hour = settings->wake_hour;
    wake_time_local.tm_min = settings->wake_minute;
    wake_time_local.tm_sec = 0;
    wake_time_local.tm_isdst = -1;

    time_t wake_time = mktime(&wake_time_local);
    time_t ramp_start = wake_time - settings->ramp_duration_seconds;
    if (ramp_start <= now) {
        wake_time_local.tm_mday++;
        wake_time_local.tm_isdst = -1;
        wake_time = mktime(&wake_time_local);
        ramp_start = wake_time - settings->ramp_duration_seconds;
    }
    return ramp_start;
}

static bool parse_form_integer(const char *body, const char *key, long minimum, long maximum, long *value)
{
    char text[16];
    if (httpd_query_key_value(body, key, text, sizeof(text)) != ESP_OK) {
        return false;
    }

    char *end;
    const long parsed = strtol(text, &end, 10);
    if (end == text || *end != '\0' || parsed < minimum || parsed > maximum) {
        return false;
    }
    *value = parsed;
    return true;
}

static esp_err_t dashboard_get_handler(httpd_req_t *request)
{
    schedule_settings_t settings;
    xSemaphoreTake(schedule_mutex, portMAX_DELAY);
    settings = schedule_settings;
    const app_state_t state = app_state;
    const time_t ramp_start = next_ramp_start_time;
    const bool synced = clock_synchronized;
    xSemaphoreGive(schedule_mutex);

    char current_time[24] = "Not synchronized";
    char ramp_start_text[24] = "Not scheduled";
    if (synced) {
        struct tm local_time;
        time_t now = time(NULL);
        localtime_r(&now, &local_time);
        strftime(current_time, sizeof(current_time), "%Y-%m-%d %H:%M:%S", &local_time);
        if (ramp_start != 0) {
            localtime_r(&ramp_start, &local_time);
            strftime(ramp_start_text, sizeof(ramp_start_text), "%Y-%m-%d %H:%M:%S", &local_time);
        }
    }

    char ip_text[16] = "Unknown";
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        snprintf(ip_text, sizeof(ip_text), IPSTR, IP2STR(&ip_info.ip));
    }

    char page[2400];
    snprintf(page, sizeof(page),
             "<!doctype html><html lang='en'><head><meta charset='utf-8'>"
             "<meta name='viewport' content='width=device-width,initial-scale=1'>"
             "<meta http-equiv='refresh' content='15'><title>Wakeup light</title>"
             "<style>:root{color-scheme:dark}body{font:16px sans-serif;max-width:640px;margin:2rem auto;padding:0 1rem;"
             "background:#101713;color:#e6eee8}dl{display:grid;grid-template-columns:10rem 1fr;gap:.6rem}dt{color:#a9b8ad}"
             "a{color:#92d5aa}form{display:grid;gap:.8rem;max-width:24rem;margin-top:2rem}label{display:grid;gap:.4rem}"
             "input,button{box-sizing:border-box;width:100%%;min-height:48px;font:inherit;padding:.7rem;border-radius:6px}"
             "input{color:#f0f5f1;background:#1b2720;border:1px solid #526457}button{color:#102016;"
             "background:#a6dfb2;border:0;font-weight:700;cursor:pointer}small{color:#a9b8ad}</style></head><body>"
             "<h1>Wakeup light</h1><dl><dt>Status</dt><dd>%s</dd>"
             "<dt>Wi-Fi IP</dt><dd>%s</dd><dt>Local time</dt><dd>%s</dd>"
             "<dt>Local URL</dt><dd><a href='http://" DEVICE_HOSTNAME ".local/'>http://" DEVICE_HOSTNAME ".local</a></dd>"
             "<dt>Next ramp starts</dt><dd>%s</dd></dl>"
             "<form method='post' action='/settings'><label>Wake hour (0-23)"
             "<input type='number' name='wake_hour' min='0' max='23' value='%u' required></label>"
             "<label>Wake minute (0-59)<input type='number' name='wake_minute' min='0' max='59' value='%u' required></label>"
             "<label>Ramp duration in seconds (2-21600)"
             "<input type='number' name='ramp_seconds' min='2' max='21600' value='%lu' required></label>"
             "<small>5 min = 300 sec | 10 min = 600 sec | 30 min = 1800 sec</small>"
             "<button type='submit'>Save schedule</button></form>"
             "<p>Wake time is when the ramp finishes. Changes are saved on the device.</p>"
             "</body></html>",
             app_state_name(state), ip_text, current_time, ramp_start_text,
             settings.wake_hour, settings.wake_minute, (unsigned long)settings.ramp_duration_seconds);

    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_send(request, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t settings_post_handler(httpd_req_t *request)
{
    if (request->content_len == 0 || request->content_len >= 128) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid form data");
    }

    char body[128];
    size_t received = 0;
    while (received < request->content_len) {
        const int result = httpd_req_recv(request, body + received, request->content_len - received);
        if (result <= 0) {
            return ESP_FAIL;
        }
        received += (size_t)result;
    }
    body[received] = '\0';

    long hour;
    long minute;
    long ramp_seconds;
    if (!parse_form_integer(body, "wake_hour", 0, 23, &hour) ||
        !parse_form_integer(body, "wake_minute", 0, 59, &minute) ||
        !parse_form_integer(body, "ramp_seconds", 2, MAX_RAMP_DURATION_SECONDS, &ramp_seconds)) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid schedule values");
    }

    const schedule_settings_t settings = {
        .wake_hour = (uint8_t)hour,
        .wake_minute = (uint8_t)minute,
        .ramp_duration_seconds = (uint32_t)ramp_seconds,
    };
    if (save_schedule_settings(&settings) != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save schedule");
    }

    httpd_resp_set_status(request, "303 See Other");
    httpd_resp_set_hdr(request, "Location", "/");
    return httpd_resp_sendstr(request, "");
}

static void start_web_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 2;
    config.stack_size = 6144;
    ESP_ERROR_CHECK(httpd_start(&web_server, &config));

    const httpd_uri_t dashboard_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = dashboard_get_handler,
    };
    const httpd_uri_t settings_uri = {
        .uri = "/settings",
        .method = HTTP_POST,
        .handler = settings_post_handler,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(web_server, &dashboard_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(web_server, &settings_uri));
    ESP_LOGI(TAG, "Wakeup settings page is ready on the device IP, port 80");
}

static void start_mdns(void)
{
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(DEVICE_HOSTNAME));
    ESP_ERROR_CHECK(mdns_instance_name_set("Wakeup Light"));
    ESP_LOGI(TAG, "Local URL: http://%s.local/", DEVICE_HOSTNAME);
}

void app_main(void)
{
    led_strip_handle_t led_strip;
    const led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .flags.with_dma = false,
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    ESP_ERROR_CHECK(led_strip_set_pixel(led_strip, 0, 0, 0, 0));
    ESP_ERROR_CHECK(led_strip_refresh(led_strip));

    if (WIFI_SSID[0] == '\0' || strlen(WIFI_SSID) > 32 || strlen(WIFI_PASSWORD) > 63) {
        ESP_LOGE(TAG, "Set valid Wi-Fi credentials in main/wakeup_config.h");
        return;
    }

    ESP_LOGI(TAG, "Starting Wi-Fi to synchronize time");
    start_wifi();
    wait_for_wifi();
    set_app_state(APP_STATE_SYNCING_TIME);
    start_mdns();
    start_web_server();

    while (!synchronize_clock()) {
        ESP_LOGW(TAG, "Time sync failed; retrying in 60 seconds");
        vTaskDelay(pdMS_TO_TICKS(60000));
    }

    setenv("TZ", TIMEZONE_RULE, 1);
    tzset();
    xSemaphoreTake(schedule_mutex, portMAX_DELAY);
    clock_synchronized = true;
    xSemaphoreGive(schedule_mutex);

    while (true) {
        schedule_settings_t settings;
        uint32_t generation;
        get_schedule_snapshot(&settings, &generation);
        time_t ramp_start = get_next_ramp_start(time(NULL), &settings);

        xSemaphoreTake(schedule_mutex, portMAX_DELAY);
        next_ramp_start_time = ramp_start;
        app_state = APP_STATE_WAITING;
        xSemaphoreGive(schedule_mutex);

        while (time(NULL) < ramp_start) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            uint32_t current_generation;
            get_schedule_snapshot(&settings, &current_generation);
            if (current_generation != generation) {
                generation = current_generation;
                ramp_start = get_next_ramp_start(time(NULL), &settings);
                xSemaphoreTake(schedule_mutex, portMAX_DELAY);
                next_ramp_start_time = ramp_start;
                xSemaphoreGive(schedule_mutex);
                ESP_LOGI(TAG, "Schedule changed; next ramp start recalculated");
            }
        }

        uint32_t current_generation;
        get_schedule_snapshot(&settings, &current_generation);
        if (current_generation != generation) {
            continue;
        }

        set_app_state(APP_STATE_RAMPING);
        ESP_LOGI(TAG, "Starting wake-up ramp");
        run_ramp(led_strip, settings.ramp_duration_seconds);
        set_app_state(APP_STATE_COMPLETE);
        ESP_LOGI(TAG, "Wake-up ramp complete");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(60000));
        }
    }
}