/*
 * road_event.c — see road_event.h for the overview.
 *
 * Shared by the ESP32 firmware and the Python training pipeline.
 * If you change anything here, retrain (python ml/train.py) so the
 * model is fitted on the new features.
 */

#include "road_event.h"

#include <math.h>
#include <string.h>

#define EPS             1e-6f
#define FIRST_PEAK_FRAC 0.6f    /* first excursion above 0.6 * trigger  */
#define FIRST_PEAK_SPAN 15      /* look 150 ms ahead for that peak      */
#define ACTIVE_G        0.15f   /* "something is happening" level       */
#define ZC_HYST_G       0.05f   /* zero-crossing hysteresis             */

const char *const RE_FEATURE_NAMES[RE_NUM_FEATURES] = {
    "v_max",            /*  0 largest upward vertical accel (g)         */
    "v_min",            /*  1 largest downward vertical accel (g)       */
    "v_p2p",            /*  2 peak-to-peak (g)                           */
    "v_std",            /*  3                                            */
    "v_mean",           /*  4                                            */
    "first_peak",       /*  5 signed first big excursion: <0 = drop      */
                        /*    first (pothole), >0 = lift first (hump)    */
    "t_max_minus_t_min",/*  6 seconds; >0 when the dip comes first       */
    "active_time",      /*  7 seconds with |v| > 0.15 g                  */
    "jerk_max",         /*  8 g/s; potholes have sharp edges             */
    "hf_ratio",         /*  9 high-frequency share of the energy         */
    "crest",            /* 10 peak / rms                                 */
    "h_max",            /* 11 horizontal dynamic accel (g)               */
    "h_std",            /* 12                                            */
    "h_v_ratio",        /* 13 horizontal rms / vertical rms              */
    "zcr",              /* 14 zero crossings per second                  */
    "pre_rms",          /* 15 road roughness before the event            */
    "neg_area",         /* 16 g*s below zero                             */
    "pos_area",         /* 17 g*s above zero                             */
    "skew",             /* 18                                            */
    "kurtosis",         /* 19                                            */
};


/* ============================================================
 * DETECTOR
 * ============================================================ */

void road_event_init(re_detector_t *d)
{
    memset(d, 0, sizeof(*d));
}


static float vertical_dyn(const float *g, float x, float y, float z)
{
    float gm = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);

    if (gm < EPS)
    {
        return 0.0f;
    }

    return (x * g[0] + y * g[1] + z * g[2]) / gm - gm;
}


int road_event_push(re_detector_t *d, float x, float y, float z)
{
    int result = RE_IDLE;

    d->rx[d->head] = x;
    d->ry[d->head] = y;
    d->rz[d->head] = z;
    d->head = (d->head + 1) % RE_WIN;

    if (d->seq == 0)
    {
        d->g[0] = x;
        d->g[1] = y;
        d->g[2] = z;
    }

    float v = vertical_dyn(d->g, x, y, z);

    /*
     * Gravity tracking. While something is happening, adapt 10x slower
     * so the event itself does not leak into the gravity estimate, but
     * still adapt eventually if the mounting angle really changed.
     */
    float alpha = RE_GRAVITY_ALPHA;

    if (d->seq < RE_WARMUP)
    {
        /* converge fast from any start value (e.g. an MPU-6050 that
         * returns 0 g for its first sample after reset) */
        alpha = 0.05f;
    }
    else if (fabsf(v) > RE_TRIGGER_G || d->remaining > 0)
    {
        alpha *= 0.1f;
    }

    d->g[0] += alpha * (x - d->g[0]);
    d->g[1] += alpha * (y - d->g[1]);
    d->g[2] += alpha * (z - d->g[2]);

    if (d->remaining > 0)
    {
        d->remaining--;

        if (d->remaining == 0)
        {
            d->cooldown = RE_COOLDOWN;
            result = RE_WINDOW_READY;
        }
    }
    else if (d->cooldown > 0)
    {
        d->cooldown--;
    }
    else if (d->seq >= RE_WARMUP && fabsf(v) > RE_TRIGGER_G)
    {
        d->trigger_seq = d->seq;
        memcpy(d->g_snap, d->g, sizeof(d->g_snap));
        d->remaining = RE_POST - 1;
        result = RE_TRIGGERED;
    }

    d->seq++;

    return result;
}


void road_event_get_window(const re_detector_t *d, re_window_t *w)
{
    /* head points at the oldest sample once the ring is full */
    for (int i = 0; i < RE_WIN; i++)
    {
        int k = (d->head + i) % RE_WIN;

        w->x[i] = d->rx[k];
        w->y[i] = d->ry[k];
        w->z[i] = d->rz[k];
    }

    memcpy(w->g, d->g_snap, sizeof(w->g));
    w->trigger_seq = d->trigger_seq;
}


/* ============================================================
 * FEATURES
 * ============================================================ */

static void decompose(const re_window_t *w, float *v, float *h)
{
    float gm = sqrtf(w->g[0] * w->g[0] + w->g[1] * w->g[1] + w->g[2] * w->g[2]);

    if (gm < EPS)
    {
        gm = 1.0f;
    }

    float ux = w->g[0] / gm;
    float uy = w->g[1] / gm;
    float uz = w->g[2] / gm;

    for (int i = 0; i < RE_WIN; i++)
    {
        float dx = w->x[i] - w->g[0];
        float dy = w->y[i] - w->g[1];
        float dz = w->z[i] - w->g[2];

        float vi = dx * ux + dy * uy + dz * uz;

        float hx = dx - vi * ux;
        float hy = dy - vi * uy;
        float hz = dz - vi * uz;

        v[i] = vi;
        h[i] = sqrtf(hx * hx + hy * hy + hz * hz);
    }
}


float road_event_severity(const re_window_t *w)
{
    float v[RE_WIN];
    float h[RE_WIN];

    decompose(w, v, h);

    float lo = v[0];
    float hi = v[0];

    for (int i = 1; i < RE_WIN; i++)
    {
        if (v[i] < lo) lo = v[i];
        if (v[i] > hi) hi = v[i];
    }

    return hi - lo;
}


void road_event_features(const re_window_t *w, float *f)
{
    const float fs = (float)RE_FS_HZ;

    float v[RE_WIN];
    float h[RE_WIN];

    decompose(w, v, h);

    /* ---------- basic statistics ---------- */

    float v_sum = 0.0f, v_sq = 0.0f, h_sum = 0.0f, h_sq = 0.0f;
    float v_max = v[0], v_min = v[0], h_max = h[0];
    int i_max = 0, i_min = 0;
    float neg_area = 0.0f, pos_area = 0.0f;
    int active = 0;
    float jerk_max = 0.0f;

    for (int i = 0; i < RE_WIN; i++)
    {
        v_sum += v[i];
        v_sq += v[i] * v[i];
        h_sum += h[i];
        h_sq += h[i] * h[i];

        if (v[i] > v_max) { v_max = v[i]; i_max = i; }
        if (v[i] < v_min) { v_min = v[i]; i_min = i; }
        if (h[i] > h_max) { h_max = h[i]; }

        if (v[i] < 0.0f) neg_area -= v[i];
        else             pos_area += v[i];

        if (fabsf(v[i]) > ACTIVE_G) active++;

        if (i > 0)
        {
            float j = fabsf(v[i] - v[i - 1]) * fs;
            if (j > jerk_max) jerk_max = j;
        }
    }

    float n = (float)RE_WIN;
    float v_mean = v_sum / n;
    float h_mean = h_sum / n;
    float v_rms = sqrtf(v_sq / n);
    float h_rms = sqrtf(h_sq / n);

    float m2 = 0.0f, m3 = 0.0f, m4 = 0.0f, hv = 0.0f;

    for (int i = 0; i < RE_WIN; i++)
    {
        float dv = v[i] - v_mean;
        float dh = h[i] - h_mean;
        float d2 = dv * dv;

        m2 += d2;
        m3 += d2 * dv;
        m4 += d2 * d2;
        hv += dh * dh;
    }

    m2 /= n;
    m3 /= n;
    m4 /= n;

    float v_std = sqrtf(m2);
    float h_std = sqrtf(hv / n);

    /* ---------- shape: which way does the road go first? ---------- */

    float first_peak = 0.0f;

    for (int i = 0; i < RE_WIN; i++)
    {
        if (fabsf(v[i]) > FIRST_PEAK_FRAC * RE_TRIGGER_G)
        {
            first_peak = v[i];

            int end = i + FIRST_PEAK_SPAN;
            if (end > RE_WIN) end = RE_WIN;

            for (int k = i; k < end; k++)
            {
                if ((v[k] > 0.0f) == (v[i] > 0.0f) &&
                    fabsf(v[k]) > fabsf(first_peak))
                {
                    first_peak = v[k];
                }
            }

            break;
        }
    }

    /* ---------- smoothness ---------- */

    float hf = 0.0f, tot = 0.0f;

    for (int i = 1; i < RE_WIN - 1; i++)
    {
        float s = (v[i - 1] + v[i] + v[i + 1]) / 3.0f;
        float r = v[i] - s;

        hf += r * r;
        tot += v[i] * v[i];
    }

    /* zero crossings with hysteresis */
    int crossings = 0;
    int state = 0;

    for (int i = 0; i < RE_WIN; i++)
    {
        int s = 0;

        if (v[i] > ZC_HYST_G) s = 1;
        else if (v[i] < -ZC_HYST_G) s = -1;

        if (s != 0)
        {
            if (state != 0 && s != state) crossings++;
            state = s;
        }
    }

    /* road roughness before the event (excluding the run-up) */
    float pre_sq = 0.0f;
    int pre_n = RE_PRE - 5;

    for (int i = 0; i < pre_n; i++)
    {
        pre_sq += v[i] * v[i];
    }

    float peak = fabsf(v_max) > fabsf(v_min) ? fabsf(v_max) : fabsf(v_min);

    f[0]  = v_max;
    f[1]  = v_min;
    f[2]  = v_max - v_min;
    f[3]  = v_std;
    f[4]  = v_mean;
    f[5]  = first_peak;
    f[6]  = (float)(i_max - i_min) / fs;
    f[7]  = (float)active / fs;
    f[8]  = jerk_max;
    f[9]  = hf / (tot + EPS);
    f[10] = peak / (v_rms + EPS);
    f[11] = h_max;
    f[12] = h_std;
    f[13] = h_rms / (v_rms + EPS);
    f[14] = (float)crossings * fs / n;
    f[15] = sqrtf(pre_sq / (float)pre_n);
    f[16] = neg_area / fs;
    f[17] = pos_area / fs;
    f[18] = m3 / (m2 * v_std + EPS);
    f[19] = m4 / (m2 * m2 + EPS);
}
