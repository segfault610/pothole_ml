/*
 * ESP32 POTHOLE DATA COLLECTOR
 *
 * Streams every accelerometer sample (100 Hz, from the ADXL345 FIFO) and
 * the GPS state over USB serial. ml/collect.py records the stream into
 * a session file; labelling and event extraction happen on the laptop.
 *
 * Serial protocol (one record per line):
 *
 *   D,<seq>,<x>,<y>,<z>                 sample, acceleration in g
 *   G,<fix>,<lat>,<lon>,<kmh>,<sats>,<hdop>   once per second
 *   E,<trigger_seq>,<class>,<confidence>      live preview of the on-device
 *                                             detector + current model
 *   W,<text>                            warning (e.g. FIFO overflow)
 *   # ...                               human-readable info, ignored
 *
 * This firmware does NOT need Wi-Fi.
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

static const char *CLASS_NAMES[] = { "normal", "speed_hump", "pothole" };


static void print_gps(void)
{
    gps_state_t s;
    gps_get(&s);

    if (!gps_receiving(&s))
    {
        printf("G,nodata,0,0,0,0,0\n");
        return;
    }

    printf("G,%d,%.7f,%.7f,%.1f,%d,%.1f\n",
           gps_fix_fresh(&s, 2000) ? 1 : 0,
           s.fix.lat, s.fix.lon, (double)s.fix.speed_kmh,
           s.fix.sats, (double)s.fix.hdop);
}


void app_main(void)
{
    printf("\n# ========================================\n");
    printf("#   ESP32 POTHOLE ML DATA COLLECTOR\n");
    printf("# ========================================\n");
    printf("# ADXL345 SDA=GPIO%d SCL=GPIO%d | GPS RX=GPIO%d TX=GPIO%d\n",
           BOARD_I2C_SDA, BOARD_I2C_SCL, BOARD_GPS_RX, BOARD_GPS_TX);
    printf("# model: %s%s\n", MODEL_NAME, MODEL_IS_SYNTHETIC ? " (SYNTHETIC placeholder)" : "");

    if (gps_init(BOARD_GPS_UART, BOARD_GPS_RX, BOARD_GPS_TX, BOARD_GPS_BAUD, true) != ESP_OK)
    {
        printf("W,gps init failed\n");
    }

    if (adxl345_init(BOARD_I2C_PORT, BOARD_I2C_SDA, BOARD_I2C_SCL) != ESP_OK)
    {
        printf("W,ADXL345 init failed - check wiring (docs/01-hardware.md)\n");

        while (1)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    printf("DATA_BEGIN\n");

    static re_detector_t det;
    static re_window_t win;
    road_event_init(&det);

    adxl345_raw_t buf[ADXL345_FIFO_DEPTH];
    uint32_t seq = 0;
    int64_t next_gps = 0;

    while (1)
    {
        int overflow = 0;
        int n = adxl345_read_fifo(buf, ADXL345_FIFO_DEPTH, &overflow);

        if (n < 0)
        {
            printf("W,i2c read error\n");
        }

        if (overflow)
        {
            printf("W,fifo overflow at %lu\n", (unsigned long)seq);
        }

        for (int i = 0; i < n; i++)
        {
            float x = buf[i].x * ADXL345_LSB_G;
            float y = buf[i].y * ADXL345_LSB_G;
            float z = buf[i].z * ADXL345_LSB_G;

            printf("D,%lu,%.4f,%.4f,%.4f\n", (unsigned long)seq, x, y, z);
            seq++;

            if (road_event_push(&det, x, y, z) == RE_WINDOW_READY)
            {
                float f[RE_NUM_FEATURES];
                float proba[MODEL_NUM_CLASSES];

                road_event_get_window(&det, &win);
                road_event_features(&win, f);
                int c = pothole_predict_proba(f, proba);

                printf("E,%lu,%s,%.2f\n", (unsigned long)win.trigger_seq,
                       CLASS_NAMES[c], (double)proba[c]);
            }
        }

        int64_t now = esp_timer_get_time();

        if (now >= next_gps)
        {
            print_gps();
            next_gps = now + 1000000;
        }

        /* FIFO holds 320 ms; draining every 40 ms leaves a big margin */
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}
