/*
 * nmea — tiny NMEA-0183 parser for RMC and GGA sentences.
 *
 * Pure C, no ESP-IDF dependencies (unit-tested on the host by
 * ml/tests/test_nmea.py). Accepts any talker id ($GP, $GN, $GL ...).
 */

#ifndef NMEA_H
#define NMEA_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NMEA_BAD_CHECKSUM   -1
#define NMEA_IGNORED        0
#define NMEA_RMC            1
#define NMEA_GGA            2

typedef struct
{
    bool valid;             /* RMC status 'A' and GGA quality > 0      */
    double lat;             /* degrees, + = north                       */
    double lon;             /* degrees, + = east                        */
    float speed_kmh;
    float course_deg;
    float hdop;
    float alt_m;
    int sats;
    int quality;            /* GGA fix quality, 0 = no fix              */

    bool has_time;
    bool has_date;
    int year, month, day;
    int hour, minute;
    float second;
} nmea_fix_t;

/*
 * Parse one sentence (with or without trailing CR/LF) and update fix.
 * Returns NMEA_RMC, NMEA_GGA, NMEA_IGNORED or NMEA_BAD_CHECKSUM.
 */
int nmea_parse_line(const char *line, nmea_fix_t *fix);

#ifdef __cplusplus
}
#endif

#endif
