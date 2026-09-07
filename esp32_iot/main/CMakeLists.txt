idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS
        "."
        "../../model"
    REQUIRES
        nvs_flash
        esp_wifi
        esp_driver_i2c
        mqtt
    EMBED_TXTFILES
        "../certs/aws_root_ca.pem"
        "../certs/certificate.pem.crt"
        "../certs/private.pem.key"
)
