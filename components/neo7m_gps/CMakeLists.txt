idf_component_register(
    SRCS "nmea.c" "gps_neo7m.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_uart esp_timer
)
