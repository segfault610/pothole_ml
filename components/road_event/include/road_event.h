/*
 * road_event — road-event detector + feature extractor.
 *
 * Pure C99, no ESP-IDF dependencies. The exact same source file is
 * compiled into the ESP32 firmware AND into a host shared library that
 * the Python training pipeline loads through ctypes (ml/road_event.py),
 * so training features are bit-for-bit what the device computes.
 *
 * How it works
 * ------------
 *  1. Every accelerometer sample (in g, any orientation) is pushed in.
 *  2. A slow low-pass filter tracks the gravity vector, so the sensor
 *     can be mounted at any angle.
 *  3. "Vertical dynamic acceleration" v = (a . g_hat) - |g| is computed.
 *  4. When |v| exceeds RE_TRIGGER_G an event is triggered. The window
 *     is RE_PRE samples before the trigger + RE_POST samples after it,
 *     so the whole bump/pothole (front AND rear axle) is captured with
 *     the trigger at a known position.
 *  5. When the window is complete, road_event_features() turns it into
 *     RE_NUM_FEATURES numbers that the classifier consumes.
 *
 * Windows where nothing triggers are, by definition, flat road and are
 * never classified — this is what keeps false positives and CPU low.
 */

#ifndef ROAD_EVENT_H
#define ROAD_EVENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RE_FS_HZ            100     /* sample rate the detector expects  */
#define RE_PRE              50      /* samples kept before the trigger   */
#define RE_POST             100     /* samples captured after trigger    */
#define RE_WIN              (RE_PRE + RE_POST)

#define RE_TRIGGER_G        0.25f   /* |vertical dyn. accel| to trigger  */
#define RE_COOLDOWN         30      /* samples ignored after a window    */
#define RE_WARMUP           (RE_WIN + 50)
#define RE_GRAVITY_ALPHA    0.01f   /* gravity low-pass (tau ~ 1 s)      */

#define RE_NUM_FEATURES     20

typedef struct
{
    float x[RE_WIN];
    float y[RE_WIN];
    float z[RE_WIN];
    float g[3];             /* gravity estimate at the trigger instant */
    uint32_t trigger_seq;   /* sample index of the trigger             */
} re_window_t;

typedef struct
{
    /* ring buffer of raw samples */
    float rx[RE_WIN];
    float ry[RE_WIN];
    float rz[RE_WIN];
    int head;               /* next write position                    */

    float g[3];             /* running gravity estimate               */
    float g_snap[3];

    uint32_t seq;           /* samples pushed so far                  */
    uint32_t trigger_seq;
    int remaining;          /* >0 while capturing the post-trigger    */
    int cooldown;
} re_detector_t;

/* Return codes of road_event_push() */
#define RE_IDLE             0
#define RE_TRIGGERED        1   /* trigger fired on this sample          */
#define RE_WINDOW_READY     2   /* window complete, call _get_window()   */

void road_event_init(re_detector_t *d);

/* Feed one sample (units: g). Returns RE_IDLE / RE_TRIGGERED / RE_WINDOW_READY. */
int road_event_push(re_detector_t *d, float x, float y, float z);

/* Copy the most recent completed window (oldest sample first). */
void road_event_get_window(const re_detector_t *d, re_window_t *w);

/* Compute the feature vector of a window. f must hold RE_NUM_FEATURES. */
void road_event_features(const re_window_t *w, float *f);

/* Peak-to-peak vertical acceleration of a window, in g (severity proxy). */
float road_event_severity(const re_window_t *w);

/* Feature names, index-aligned with road_event_features(). */
extern const char *const RE_FEATURE_NAMES[RE_NUM_FEATURES];

#ifdef __cplusplus
}
#endif

#endif
