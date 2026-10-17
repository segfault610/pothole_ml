/*
 * Host-side shim so Python (ctypes) can drive the firmware's own
 * road_event.c and nmea.c without mirroring their structs.
 * Built automatically by ml/road_event.py.
 */

#include <stdlib.h>
#include <string.h>

#include "road_event.h"
#include "nmea.h"

int re_const(int which)
{
    switch (which)
    {
        case 0: return RE_FS_HZ;
        case 1: return RE_PRE;
        case 2: return RE_POST;
        case 3: return RE_NUM_FEATURES;
        case 4: return (int)(RE_TRIGGER_G * 1000.0f + 0.5f);   /* milli-g */
        default: return -1;
    }
}

const char *re_feature_name(int i)
{
    return RE_FEATURE_NAMES[i];
}

void *re_new(void)
{
    re_detector_t *d = malloc(sizeof(*d));
    road_event_init(d);
    return d;
}

void re_free(void *d)
{
    free(d);
}

int re_push(void *d, float x, float y, float z)
{
    return road_event_push((re_detector_t *)d, x, y, z);
}

/* xyz: RE_WIN*3 interleaved, g: 3 */
unsigned re_window(void *d, float *xyz, float *g)
{
    re_window_t w;
    road_event_get_window((re_detector_t *)d, &w);

    for (int i = 0; i < RE_WIN; i++)
    {
        xyz[3 * i + 0] = w.x[i];
        xyz[3 * i + 1] = w.y[i];
        xyz[3 * i + 2] = w.z[i];
    }

    memcpy(g, w.g, sizeof(w.g));
    return w.trigger_seq;
}

static void fill(re_window_t *w, const float *xyz, const float *g)
{
    for (int i = 0; i < RE_WIN; i++)
    {
        w->x[i] = xyz[3 * i + 0];
        w->y[i] = xyz[3 * i + 1];
        w->z[i] = xyz[3 * i + 2];
    }

    memcpy(w->g, g, sizeof(w->g));
}

void re_features(const float *xyz, const float *g, float *f)
{
    re_window_t w;
    fill(&w, xyz, g);
    road_event_features(&w, f);
}

float re_severity(const float *xyz, const float *g)
{
    re_window_t w;
    fill(&w, xyz, g);
    return road_event_severity(&w);
}

/* out: lat, lon, speed_kmh, hdop, sats, quality, valid */
int nmea_host_parse(const char *line, double *out)
{
    static nmea_fix_t fix;
    int r = nmea_parse_line(line, &fix);

    out[0] = fix.lat;
    out[1] = fix.lon;
    out[2] = fix.speed_kmh;
    out[3] = fix.hdop;
    out[4] = fix.sats;
    out[5] = fix.quality;
    out[6] = fix.valid;
    return r;
}
