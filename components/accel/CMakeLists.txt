idf_component_register(
    SRCS "accel.c" "adxl345.c" "mpu6050.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES esp_driver_i2c
    PRIV_REQUIRES esp_driver_gpio
)
