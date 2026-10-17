/*
 * gps_neo7m — u-blox NEO-7M over UART.
 *
 * A background task reads NMEA, keeps the latest fix and makes it
 * available to other tasks through gps_get(). Optionally sends UBX
 * commands at boot to switch the module to 5 Hz and drop the NMEA
 * sentences we do not use (keeps 9600 baud from saturating).
 */

#ifndef GPS_NEO7M_H
#define GPS_NEO7M_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "nmea.h"

typedef struct
{
    nmea_fix_t fix;
    int64_t fix_us;         /* esp_timer time of the last valid fix     */
    int64_t rx_us;          /* esp_timer time of the last sentence      */
    uint32_t sentences;
    uint32_t checksum_errors;
} gps_state_t;

esp_err_t gps_init(int uart_num, int rx_pin, int tx_pin, int baud, bool rate_5hz);

/* Thread-safe copy of the current state. */
void gps_get(gps_state_t *out);

/* True if a valid fix is no older than max_age_ms. */
bool gps_fix_fresh(const gps_state_t *s, int max_age_ms);

/* True if any NMEA sentence arrived in the last 2 s (wiring check). */
bool gps_receiving(const gps_state_t *s);

/* ISO-8601 UTC timestamp of the fix ("" if the GPS has no date yet). */
void gps_format_utc(const gps_state_t *s, char *buf, size_t len);

#endif
