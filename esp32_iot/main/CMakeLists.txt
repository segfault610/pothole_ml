idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        board
        accel
        neo7m_gps
        road_event
        pothole_model
        nvs_flash
        esp_wifi
        esp_driver_gpio
        mqtt
    EMBED_TXTFILES
        "../certs/aws_root_ca.pem"
        "../certs/certificate.pem.crt"
        "../certs/private.pem.key"
)
