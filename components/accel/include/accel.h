/*
 * accel — accelerometer front-end with auto-detection.
 *
 * Supported (whichever answers on the I2C bus, same wiring):
 *   ADXL345 (GY-291)  address 0x53 (SDO->GND) or 0x1D
 *   MPU-6050 (GY-521) address 0x68 (AD0->GND) or 0x69
 *
 * Both run at 100 Hz from their own sample clock with a ~45-50 Hz low-pass
 * and a +/-8 g range (ADXL345: hardware FIFO; MPU-6050: DATA_RDY polling,
 * see mpu6050.c for why). Samples come out in g, so
 * the road_event detector and the model do not care which sensor is used.
 */

#ifndef ACCEL_H
#define ACCEL_H

#include "esp_err.h"

#define ACCEL_RATE_HZ       100
#define ACCEL_MAX_BATCH     32      /* read at least every 300 ms */

typedef struct
{
    float x;
    float y;
    float z;
} accel_sample_t;

/* Create the I2C bus (first call) and detect a sensor. Safe to call again. */
esp_err_t accel_init(int i2c_port, int sda, int scl);

/* "ADXL345" / "MPU-6050" / "none" */
const char *accel_name(void);

/*
 * Read every buffered sample (at most max), in g.
 * Returns the number of samples, or -1 on bus error.
 * *overflow is set when the sensor FIFO overflowed (samples were lost).
 */
int accel_read(accel_sample_t *out, int max, int *overflow);

#endif
