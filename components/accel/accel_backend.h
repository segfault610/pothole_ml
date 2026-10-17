/* Internal interface between accel.c and the sensor drivers. */

#ifndef ACCEL_BACKEND_H
#define ACCEL_BACKEND_H

#include "driver/i2c_master.h"
#include "accel.h"

typedef struct
{
    const char *name;
    /* probe + configure; ESP_ERR_NOT_FOUND if the sensor is not present */
    esp_err_t (*init)(i2c_master_bus_handle_t bus);
    int (*read)(accel_sample_t *out, int max, int *overflow);
} accel_backend_t;

extern const accel_backend_t adxl345_backend;
extern const accel_backend_t mpu6050_backend;

#endif
