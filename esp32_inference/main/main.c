/*
 * ESP32 POTHOLE ML INFERENCE (bench / offline test)
 *
 * Same detection + classification as esp32_iot, but without Wi-Fi/MQTT:
 * every detected road event is printed with its class, confidence,
 * features and the current GPS position. Use it to check the wiring and
 * a freshly trained model before deploying esp32_iot.
 *
 * Unlike the old version there is no countdown: sampling is continuous
 * and the detector decides when something happened. On the bench, a
 * sharp tap or a quick dip-and-lift of the sensor triggers an event.
 */

#include <stdio.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_timer.h"

#include "adxl345.h"
#include "board_pins.h"
#include "gps_neo7m.h"
#include "pothole_model.h"
#include "road_event.h"

static const char *CLASS_NAMES[] = { "NORMAL", "SPEED HUMP", "POTHOLE" };


static void print_status(void)
{
    gps_state_t s;
    gps_get(&s);

    if (!gps_receiving(&s))
    {
        printf("[gps] no NMEA data - check GPS TX -> GPIO%d and power\n", BOARD_GPS_RX);
    }
    else if (!gps_fix_fresh(&s, 2000))
    {
        printf("[gps] receiving, waiting for fix (sats=%d)\n", s.fix.sats);
    }
    else
    {
        printf("[gps] fix %.6f, %.6f  %.1f km/h  sats=%d hdop=%.1f\n",
               s.fix.lat, s.fix.lon, (double)s.fix.speed_kmh, s.fix.sats, (double)s.fix.hdop);
    }
}


static void report_event(const re_window_t *w)
{
    float f[RE_NUM_FEATURES];
    float proba[MODEL_NUM_CLASSES];

    road_event_features(w, f);
    int c = pothole_predict_proba(f, proba);

    printf("\n========================================\n");
    printf(" EVENT @ sample %lu -> %s (%.0f%%)\n",
           (unsigned long)w->trigger_seq, CLASS_NAMES[c], (double)(proba[c] * 100.0f));
    printf("   p(normal)=%.2f p(hump)=%.2f p(pothole)=%.2f  severity=%.2f g\n",
           (double)proba[CLASS_NORMAL], (double)proba[CLASS_SPEED_HUMP],
           (double)proba[CLASS_POTHOLE], (double)road_event_severity(w));
    printf("----------------------------------------\n");

    for (int i = 0; i < RE_NUM_FEATURES; i++)
    {
        printf("   %-18s % .4f\n", RE_FEATURE_NAMES[i], (double)f[i]);
    }

    print_status();
    printf("========================================\n");
}


void app_main(void)
{
    printf("\n========================================\n");
    printf("      ESP32 POTHOLE ML INFERENCE\n");
    printf("========================================\n");
    printf("model    : %s, %d tree(s)\n", MODEL_NAME, MODEL_NUM_TREES);

    if (MODEL_IS_SYNTHETIC)
    {
        printf("WARNING  : model trained on SYNTHETIC data - retrain with real drives\n");
    }

    printf("trigger  : |vertical accel| > %.2f g\n", (double)RE_TRIGGER_G);

    gps_init(BOARD_GPS_UART, BOARD_GPS_RX, BOARD_GPS_TX, BOARD_GPS_BAUD, true);

    if (adxl345_init(BOARD_I2C_PORT, BOARD_I2C_SDA, BOARD_I2C_SCL) != ESP_OK)
    {
        printf("ADXL345 initialization failed - check wiring (docs/01-hardware.md)\n");
        return;
    }

    static re_detector_t det;
    static re_window_t win;
    road_event_init(&det);

    adxl345_raw_t buf[ADXL345_FIFO_DEPTH];
    int64_t next_status = 0;

    printf("calibrating gravity, keep still for 2 s...\n");

    while (1)
    {
        int overflow = 0;
        int n = adxl345_read_fifo(buf, ADXL345_FIFO_DEPTH, &overflow);

        if (overflow)
        {
            printf("warning: FIFO overflow, samples lost\n");
        }

        for (int i = 0; i < n; i++)
        {
            int r = road_event_push(&det,
                                    buf[i].x * ADXL345_LSB_G,
                                    buf[i].y * ADXL345_LSB_G,
                                    buf[i].z * ADXL345_LSB_G);

            if (det.seq == RE_WARMUP)
            {
                printf("ready - waiting for road events\n");
            }

            if (r == RE_WINDOW_READY)
            {
                road_event_get_window(&det, &win);
                report_event(&win);
            }
        }

        int64_t now = esp_timer_get_time();

        if (now >= next_status)
        {
            print_status();
            next_status = now + 10000000;
        }

        vTaskDelay(pdMS_TO_TICKS(40));
    }
}
