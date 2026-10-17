/*
 * adxl345 — minimal ADXL345 driver (I2C, FIFO stream mode).
 *
 * The sensor samples on its own clock at exactly 100 Hz and buffers up
 * to 32 samples in its FIFO. The application drains the FIFO every
 * few tens of milliseconds, so sample timing never depends on FreeRTOS
 * scheduling (the old firmware's vTaskDelay(<10 ms) silently became a
 * zero-tick delay at CONFIG_FREERTOS_HZ=100, which broke the timing).
 */

#ifndef ADXL345_H
#define ADXL345_H

#include <stdint.h>
#include "esp_err.h"

#define ADXL345_ADDR        0x53
#define ADXL345_LSB_G       0.0039f     /* full resolution: 3.9 mg/LSB */
#define ADXL345_FIFO_DEPTH  32

typedef struct
{
    int16_t x;
    int16_t y;
    int16_t z;
} adxl345_raw_t;

/* Initialise I2C + sensor: 100 Hz, +/-8 g full resolution, FIFO stream. */
esp_err_t adxl345_init(int i2c_port, int sda, int scl);

/*
 * Read every sample currently in the FIFO (at most max).
 * Returns the number of samples written to out, or -1 on I2C error.
 * *overflow is set when the FIFO was full (samples may have been lost).
 */
int adxl345_read_fifo(adxl345_raw_t *out, int max, int *overflow);

#endif
