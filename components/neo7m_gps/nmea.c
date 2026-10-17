#include "nmea.h"

#include <stdlib.h>
#include <string.h>

#define MAX_FIELDS  24
#define MAX_LINE    120


static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}


/* "ddmm.mmmm" (lat) / "dddmm.mmmm" (lon) -> decimal degrees */
static double parse_coord(const char *s, char hemi)
{
    if (*s == '\0')
    {
        return 0.0;
    }

    double raw = atof(s);
    int deg = (int)(raw / 100.0);
    double value = deg + (raw - deg * 100.0) / 60.0;

    if (hemi == 'S' || hemi == 'W')
    {
        value = -value;
    }

    return value;
}


static void parse_time(const char *s, nmea_fix_t *fix)
{
    if (strlen(s) < 6)
    {
        return;
    }

    fix->hour = (s[0] - '0') * 10 + (s[1] - '0');
    fix->minute = (s[2] - '0') * 10 + (s[3] - '0');
    fix->second = (float)atof(s + 4);
    fix->has_time = true;
}


int nmea_parse_line(const char *line, nmea_fix_t *fix)
{
    char buf[MAX_LINE];
    char *fields[MAX_FIELDS];
    int count = 0;

    if (line[0] != '$')
    {
        return NMEA_IGNORED;
    }

    size_t len = strcspn(line, "\r\n");

    if (len >= sizeof(buf))
    {
        return NMEA_IGNORED;
    }

    memcpy(buf, line, len);
    buf[len] = '\0';

    /* checksum: XOR of everything between '$' and '*' */
    char *star = strchr(buf, '*');

    if (star == NULL || hex_val(star[1]) < 0 || hex_val(star[2]) < 0)
    {
        return NMEA_BAD_CHECKSUM;
    }

    unsigned char sum = 0;

    for (char *p = buf + 1; p < star; p++)
    {
        sum ^= (unsigned char)*p;
    }

    if (sum != (unsigned char)(hex_val(star[1]) * 16 + hex_val(star[2])))
    {
        return NMEA_BAD_CHECKSUM;
    }

    *star = '\0';

    /* split on commas, keeping empty fields */
    char *p = buf + 1;
    fields[count++] = p;

    while (*p && count < MAX_FIELDS)
    {
        if (*p == ',')
        {
            *p = '\0';
            fields[count++] = p + 1;
        }
        p++;
    }

    const char *type = fields[0];

    if (strlen(type) != 5)
    {
        return NMEA_IGNORED;
    }

    /* skip the 2-letter talker id */
    if (strcmp(type + 2, "RMC") == 0 && count >= 10)
    {
        /* 1 time, 2 status, 3 lat, 4 N/S, 5 lon, 6 E/W, 7 knots, 8 course, 9 date */
        parse_time(fields[1], fix);

        bool active = fields[2][0] == 'A';

        if (active)
        {
            fix->lat = parse_coord(fields[3], fields[4][0]);
            fix->lon = parse_coord(fields[5], fields[6][0]);
            fix->speed_kmh = (float)(atof(fields[7]) * 1.852);
            fix->course_deg = (float)atof(fields[8]);
        }

        fix->valid = active;

        if (strlen(fields[9]) == 6)
        {
            const char *d = fields[9];
            fix->day = (d[0] - '0') * 10 + (d[1] - '0');
            fix->month = (d[2] - '0') * 10 + (d[3] - '0');
            fix->year = 2000 + (d[4] - '0') * 10 + (d[5] - '0');
            fix->has_date = true;
        }

        return NMEA_RMC;
    }

    if (strcmp(type + 2, "GGA") == 0 && count >= 10)
    {
        /* 1 time, 2 lat, 3 N/S, 4 lon, 5 E/W, 6 quality, 7 sats, 8 hdop, 9 alt */
        parse_time(fields[1], fix);

        fix->quality = atoi(fields[6]);
        fix->sats = atoi(fields[7]);
        fix->hdop = (float)atof(fields[8]);

        if (fix->quality > 0)
        {
            fix->lat = parse_coord(fields[2], fields[3][0]);
            fix->lon = parse_coord(fields[4], fields[5][0]);
            fix->alt_m = (float)atof(fields[9]);
        }
        else
        {
            fix->valid = false;
        }

        return NMEA_GGA;
    }

    return NMEA_IGNORED;
}
