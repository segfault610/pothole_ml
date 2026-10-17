#include "gps_neo7m.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"

#define RX_BUF_SIZE     1024
#define NMEA_LINE_MAX        120

static const char *TAG = "GPS";

static int gps_uart = -1;
static SemaphoreHandle_t lock;
static gps_state_t state;


/* ============================================================
 * UBX CONFIGURATION
 * ============================================================ */

static void ubx_send(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t len)
{
    uint8_t msg[8 + 16];
    uint8_t ck_a = 0, ck_b = 0;

    msg[0] = 0xB5;
    msg[1] = 0x62;
    msg[2] = cls;
    msg[3] = id;
    msg[4] = len & 0xFF;
    msg[5] = len >> 8;
    memcpy(&msg[6], payload, len);

    /* 8-bit Fletcher checksum over class..payload */
    for (int i = 2; i < 6 + len; i++)
    {
        ck_a += msg[i];
        ck_b += ck_a;
    }

    msg[6 + len] = ck_a;
    msg[7 + len] = ck_b;

    uart_write_bytes(gps_uart, msg, 8 + len);
    uart_wait_tx_done(gps_uart, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(50));
}


static void configure_5hz(void)
{
    /* CFG-MSG: rate 0 for GLL, GSA, GSV, VTG -> only GGA + RMC remain */
    const uint8_t unused[] = { 0x01, 0x02, 0x03, 0x05 };

    for (size_t i = 0; i < sizeof(unused); i++)
    {
        uint8_t p[3] = { 0xF0, unused[i], 0x00 };
        ubx_send(0x06, 0x01, p, sizeof(p));
    }

    /* CFG-RATE: measRate 200 ms, navRate 1, timeRef GPS */
    uint8_t rate[6] = { 200, 0, 1, 0, 1, 0 };
    ubx_send(0x06, 0x08, rate, sizeof(rate));

    ESP_LOGI(TAG, "sent UBX config: 5 Hz, GGA+RMC only");
}


/* ============================================================
 * READER TASK
 * ============================================================ */

static void gps_task(void *arg)
{
    (void)arg;

    char line[NMEA_LINE_MAX];
    int pos = 0;
    uint8_t chunk[128];
    nmea_fix_t work;

    memset(&work, 0, sizeof(work));

    while (1)
    {
        int n = uart_read_bytes(gps_uart, chunk, sizeof(chunk), pdMS_TO_TICKS(100));

        for (int i = 0; i < n; i++)
        {
            char c = (char)chunk[i];

            if (c == '$')
            {
                pos = 0;
            }

            if (c == '\n')
            {
                line[pos] = '\0';

                xSemaphoreTake(lock, portMAX_DELAY);

                work = state.fix;
                int r = nmea_parse_line(line, &work);
                int64_t now = esp_timer_get_time();

                if (r == NMEA_BAD_CHECKSUM)
                {
                    state.checksum_errors++;
                }
                else if (r != NMEA_IGNORED)
                {
                    state.fix = work;
                    state.sentences++;
                    state.rx_us = now;

                    if (work.valid && r == NMEA_RMC)
                    {
                        state.fix_us = now;
                    }
                }

                xSemaphoreGive(lock);

                pos = 0;
            }
            else if (pos < NMEA_LINE_MAX - 1)
            {
                line[pos++] = c;
            }
        }
    }
}


/* ============================================================
 * PUBLIC API
 * ============================================================ */

esp_err_t gps_init(int uart_num, int rx_pin, int tx_pin, int baud, bool rate_5hz)
{
    uart_config_t cfg = {
        .baud_rate = baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_driver_install(uart_num, RX_BUF_SIZE, 0, 0, NULL, 0);
    if (ret != ESP_OK) return ret;

    ret = uart_param_config(uart_num, &cfg);
    if (ret != ESP_OK) return ret;

    ret = uart_set_pin(uart_num, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) return ret;

    gps_uart = uart_num;
    lock = xSemaphoreCreateMutex();
    memset(&state, 0, sizeof(state));

    if (rate_5hz)
    {
        configure_5hz();
    }

    xTaskCreate(gps_task, "gps", 3072, NULL, 5, NULL);

    ESP_LOGI(TAG, "UART%d RX=GPIO%d TX=GPIO%d %d baud", uart_num, rx_pin, tx_pin, baud);

    return ESP_OK;
}


void gps_get(gps_state_t *out)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    *out = state;
    xSemaphoreGive(lock);
}


bool gps_fix_fresh(const gps_state_t *s, int max_age_ms)
{
    if (!s->fix.valid || s->fix_us == 0)
    {
        return false;
    }

    return (esp_timer_get_time() - s->fix_us) <= (int64_t)max_age_ms * 1000;
}


bool gps_receiving(const gps_state_t *s)
{
    return s->rx_us != 0 && (esp_timer_get_time() - s->rx_us) < 2000000;
}


void gps_format_utc(const gps_state_t *s, char *buf, size_t len)
{
    const nmea_fix_t *f = &s->fix;

    if (!f->has_date || !f->has_time)
    {
        buf[0] = '\0';
        return;
    }

    snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%06.3fZ",
             f->year, f->month, f->day, f->hour, f->minute, (double)f->second);
}
