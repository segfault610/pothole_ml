/*
 * ESP32 POTHOLE IOT SYSTEM
 *
 *   ADXL345 (I2C, 100 Hz FIFO) --> road_event detector --> tree model
 *                                                            |
 *   NEO-7M (UART2, 5 Hz NMEA) --> position at the trigger ---+
 *                                                            v
 *                         queue (survives Wi-Fi drop-outs) --> AWS IoT MQTT
 *
 * Tasks
 *   sensor   core 1, high priority: drains the FIFO, detects + classifies
 *   publish  sends queued events whenever MQTT is connected
 *   led      GPIO2: off = no GPS data, blinking = no fix, on = fix,
 *            3 quick flashes = event published
 *   gps      (component) parses NMEA in the background
 *
 * Wi-Fi credentials and the AWS endpoint live in secrets.h (git-ignored).
 * Copy secrets.example.h to secrets.h and fill it in.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "mqtt_client.h"

#include "adxl345.h"
#include "board_pins.h"
#include "gps_neo7m.h"
#include "pothole_model.h"
#include "road_event.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copy esp32_iot/main/secrets.example.h to secrets.h and fill in Wi-Fi + AWS endpoint"
#endif


/* ============================================================
 * CONFIGURATION
 * ============================================================ */

#define AWS_IOT_TOPIC           "pothole/alerts"

#define PUBLISH_SPEED_HUMPS     1       /* map humps too (event field says which) */
#define MIN_CONFIDENCE          0.60f   /* below this, the event is not published */
#define GPS_MAX_AGE_MS          3000    /* older fixes are reported as no fix      */
#define EVENT_QUEUE_LEN         32      /* events kept while offline               */


/* ============================================================
 * GLOBALS
 * ============================================================ */

static const char *TAG = "POTHOLE_IOT";

extern const uint8_t aws_root_ca_pem_start[]    asm("_binary_aws_root_ca_pem_start");
extern const uint8_t certificate_pem_crt_start[] asm("_binary_certificate_pem_crt_start");
extern const uint8_t private_pem_key_start[]    asm("_binary_private_pem_key_start");

static esp_mqtt_client_handle_t mqtt_client = NULL;
static volatile bool mqtt_connected = false;
static volatile int led_flashes = 0;

static char device_id[24];
static QueueHandle_t event_queue;

typedef struct
{
    int cls;
    float confidence;
    float severity_g;
    uint32_t uptime_ms;
    gps_state_t gps;        /* GPS state at the moment of the trigger */
    int64_t gps_age_ms;
    bool gps_fix;
} road_alert_t;

static const char *EVENT_NAMES[] = { "NORMAL", "SPEED_HUMP", "POTHOLE" };


/* ============================================================
 * WIFI (non-blocking: detection runs even without network)
 * ============================================================ */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGW(TAG, "Wi-Fi disconnected, reconnecting...");
        mqtt_connected = false;
        esp_wifi_connect();
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        ESP_LOGI(TAG, "Wi-Fi connected");
    }
}


static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to Wi-Fi SSID: %s", WIFI_SSID);
}


/* ============================================================
 * AWS IOT MQTT
 * ============================================================ */

static void mqtt_event_handler(void *args, esp_event_base_t base, int32_t id, void *data)
{
    switch ((esp_mqtt_event_id_t)id)
    {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected to AWS IoT");
            mqtt_connected = true;
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected");
            mqtt_connected = false;
            break;

        default:
            break;
    }
}


static void aws_iot_init(void)
{
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address.uri = "mqtts://" AWS_IOT_ENDPOINT ":8883",
            .verification.certificate = (const char *)aws_root_ca_pem_start,
        },
        .credentials = {
            .authentication = {
                .certificate = (const char *)certificate_pem_crt_start,
                .key = (const char *)private_pem_key_start,
            },
        },
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);

    if (mqtt_client == NULL)
    {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return;
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(
        mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt_client));
}


/* ============================================================
 * PAYLOAD
 *
 * {"device_id":"pothole-a1b2c3","event":"POTHOLE","confidence":0.87,
 *  "severity_g":1.42,"coordinates":[12.9716010,79.1588020],
 *  "lat":12.9716010,"lon":79.1588020,
 *  "gps":{"fix":true,"sats":8,"hdop":1.1,"speed_kmh":32.4,"age_ms":120},
 *  "utc":"2026-10-17T10:22:33.400Z","uptime_ms":123456,"model":"..."}
 *
 * Without a fix, coordinates/lat/lon are null and gps.fix is false.
 * "coordinates" is kept for compatibility with the existing cloud logger.
 * ============================================================ */

static int format_alert(const road_alert_t *a, char *buf, size_t len)
{
    char coords[96];
    char utc[32];

    if (a->gps_fix)
    {
        snprintf(coords, sizeof(coords),
                 "\"coordinates\":[%.7f,%.7f],\"lat\":%.7f,\"lon\":%.7f",
                 a->gps.fix.lat, a->gps.fix.lon, a->gps.fix.lat, a->gps.fix.lon);
    }
    else
    {
        snprintf(coords, sizeof(coords), "\"coordinates\":null,\"lat\":null,\"lon\":null");
    }

    gps_format_utc(&a->gps, utc, sizeof(utc));

    return snprintf(buf, len,
        "{\"device_id\":\"%s\",\"event\":\"%s\",\"confidence\":%.2f,"
        "\"severity_g\":%.2f,%s,"
        "\"gps\":{\"fix\":%s,\"sats\":%d,\"hdop\":%.1f,\"speed_kmh\":%.1f,\"age_ms\":%lld},"
        "\"utc\":%s%s%s,\"uptime_ms\":%lu,\"model\":\"%s\"}",
        device_id, EVENT_NAMES[a->cls], (double)a->confidence,
        (double)a->severity_g, coords,
        a->gps_fix ? "true" : "false", a->gps.fix.sats, (double)a->gps.fix.hdop,
        (double)a->gps.fix.speed_kmh, (long long)a->gps_age_ms,
        utc[0] ? "\"" : "", utc[0] ? utc : "null", utc[0] ? "\"" : "",
        (unsigned long)a->uptime_ms, MODEL_NAME);
}


static void publish_task(void *arg)
{
    road_alert_t alert;
    char payload[448];

    while (1)
    {
        /* peek: the event leaves the queue only after a successful publish */
        if (xQueuePeek(event_queue, &alert, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        if (!mqtt_connected)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        format_alert(&alert, payload, sizeof(payload));

        int msg_id = esp_mqtt_client_publish(mqtt_client, AWS_IOT_TOPIC, payload, 0, 1, 0);

        if (msg_id < 0)
        {
            ESP_LOGE(TAG, "publish failed, will retry");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        xQueueReceive(event_queue, &alert, 0);
        led_flashes = 3;

        ESP_LOGI(TAG, "Published (msg_id=%d): %s", msg_id, payload);
    }
}


/* ============================================================
 * SENSOR + INFERENCE
 * ============================================================ */

static void sensor_task(void *arg)
{
    static re_detector_t det;
    static re_window_t win;
    static adxl345_raw_t buf[ADXL345_FIFO_DEPTH];

    gps_state_t gps_at_trigger;
    memset(&gps_at_trigger, 0, sizeof(gps_at_trigger));

    road_event_init(&det);

    while (1)
    {
        int overflow = 0;
        int n = adxl345_read_fifo(buf, ADXL345_FIFO_DEPTH, &overflow);

        if (n < 0)
        {
            ESP_LOGW(TAG, "ADXL345 read failed");
        }

        if (overflow)
        {
            ESP_LOGW(TAG, "ADXL345 FIFO overflow (samples lost)");
        }

        for (int i = 0; i < n; i++)
        {
            int r = road_event_push(&det,
                                    buf[i].x * ADXL345_LSB_G,
                                    buf[i].y * ADXL345_LSB_G,
                                    buf[i].z * ADXL345_LSB_G);

            if (r == RE_TRIGGERED)
            {
                /* remember where we were when the wheel hit it */
                gps_get(&gps_at_trigger);
            }
            else if (r == RE_WINDOW_READY)
            {
                float f[RE_NUM_FEATURES];
                float proba[MODEL_NUM_CLASSES];

                road_event_get_window(&det, &win);
                road_event_features(&win, f);

                int cls = pothole_predict_proba(f, proba);

                ESP_LOGI(TAG, "event: %s  p=[%.2f %.2f %.2f]",
                         EVENT_NAMES[cls], (double)proba[0], (double)proba[1], (double)proba[2]);

                bool wanted = cls == CLASS_POTHOLE ||
                              (PUBLISH_SPEED_HUMPS && cls == CLASS_SPEED_HUMP);

                if (!wanted || proba[cls] < MIN_CONFIDENCE)
                {
                    continue;
                }

                road_alert_t alert = {
                    .cls = cls,
                    .confidence = proba[cls],
                    .severity_g = road_event_severity(&win),
                    .uptime_ms = (uint32_t)(esp_timer_get_time() / 1000),
                    .gps = gps_at_trigger,
                    .gps_fix = gps_fix_fresh(&gps_at_trigger, GPS_MAX_AGE_MS),
                    .gps_age_ms = gps_at_trigger.fix_us
                        ? (esp_timer_get_time() - gps_at_trigger.fix_us) / 1000 : -1,
                };

                if (cls == CLASS_POTHOLE)
                {
                    ESP_LOGW(TAG, "POTHOLE DETECTED (%.0f%%, %.2f g)",
                             (double)(alert.confidence * 100.0f), (double)alert.severity_g);
                }

                if (!alert.gps_fix)
                {
                    ESP_LOGW(TAG, "no GPS fix - publishing without coordinates");
                }

                /* queue full while offline: drop the oldest event */
                if (xQueueSend(event_queue, &alert, 0) != pdTRUE)
                {
                    road_alert_t dropped;
                    xQueueReceive(event_queue, &dropped, 0);
                    xQueueSend(event_queue, &alert, 0);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(40));
    }
}


/* ============================================================
 * STATUS LED
 * ============================================================ */

static void led_task(void *arg)
{
    gpio_reset_pin(BOARD_LED);
    gpio_set_direction(BOARD_LED, GPIO_MODE_OUTPUT);

    gps_state_t s;
    int tick = 0;

    while (1)
    {
        if (led_flashes > 0)
        {
            for (int i = 0; i < led_flashes; i++)
            {
                gpio_set_level(BOARD_LED, 1);
                vTaskDelay(pdMS_TO_TICKS(60));
                gpio_set_level(BOARD_LED, 0);
                vTaskDelay(pdMS_TO_TICKS(60));
            }
            led_flashes = 0;
        }

        gps_get(&s);

        int level = 0;

        if (gps_fix_fresh(&s, GPS_MAX_AGE_MS))
        {
            level = 1;
        }
        else if (gps_receiving(&s))
        {
            level = (tick / 5) % 2;
        }

        gpio_set_level(BOARD_LED, level);
        tick++;

        if (tick % 300 == 0)
        {
            ESP_LOGI(TAG, "status: gps=%s sats=%d mqtt=%s queued=%u",
                     level == 1 ? "fix" : (gps_receiving(&s) ? "searching" : "NO DATA"),
                     s.fix.sats, mqtt_connected ? "up" : "down",
                     (unsigned)uxQueueMessagesWaiting(event_queue));
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}


/* ============================================================
 * MAIN
 * ============================================================ */

void app_main(void)
{
    printf("\n========================================\n");
    printf("       ESP32 POTHOLE IOT SYSTEM\n");
    printf("========================================\n");

    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(device_id, sizeof(device_id), "pothole-%02x%02x%02x", mac[3], mac[4], mac[5]);

    ESP_LOGI(TAG, "device %s, model %s (%d trees)%s", device_id, MODEL_NAME, MODEL_NUM_TREES,
             MODEL_IS_SYNTHETIC ? " - SYNTHETIC placeholder, retrain with real data" : "");

    event_queue = xQueueCreate(EVENT_QUEUE_LEN, sizeof(road_alert_t));

    /* sensors first: detection must not wait for the network */
    gps_init(BOARD_GPS_UART, BOARD_GPS_RX, BOARD_GPS_TX, BOARD_GPS_BAUD, true);

    if (adxl345_init(BOARD_I2C_PORT, BOARD_I2C_SDA, BOARD_I2C_SCL) != ESP_OK)
    {
        ESP_LOGE(TAG, "ADXL345 initialization failed - check wiring (docs/01-hardware.md)");
        return;
    }

    xTaskCreatePinnedToCore(sensor_task, "sensor", 6144, NULL, 10, NULL, 1);
    xTaskCreate(led_task, "led", 2560, NULL, 2, NULL);

    wifi_init_sta();
    aws_iot_init();

    xTaskCreate(publish_task, "publish", 6144, NULL, 5, NULL);
}
