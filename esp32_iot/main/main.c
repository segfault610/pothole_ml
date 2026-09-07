#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "driver/i2c_master.h"
#include "mqtt_client.h"

#include "pothole_model.h"


/* ============================================================
 * CONFIGURATION
 * ============================================================ */

/* ---------------- Wi-Fi ---------------- */

#define WIFI_SSID           "jonnys_ideapad"
#define WIFI_PASS           "Jona2jef"


/* ---------------- AWS IoT ---------------- */

#define AWS_IOT_ENDPOINT    "a30vszghfx8s3o-ats.iot.ap-south-1.amazonaws.com"
#define AWS_IOT_TOPIC       "pothole/alerts"


/* ---------------- I2C ---------------- */

#define I2C_PORT            I2C_NUM_0

#define I2C_SDA             21
#define I2C_SCL             22


/* ---------------- ADXL345 ---------------- */

#define ADXL345_ADDR        0x53

#define REG_DEVID           0x00
#define REG_BW_RATE         0x2C
#define REG_POWER_CTL       0x2D
#define REG_DATA_FORMAT     0x31
#define REG_DATAX0          0x32

#define EXPECTED_DEVID      0xE5


/* ---------------- Sampling ---------------- */

#define SAMPLE_RATE_HZ      100
#define SAMPLE_PERIOD_MS    10

#define WINDOW_SAMPLES      100
#define NUM_FEATURES        18


/* ---------------- Detection ---------------- */

/*
 * After a pothole is detected, wait this long before
 * allowing another pothole alert.
 *
 * This prevents one physical event from generating
 * many MQTT messages.
 */
#define ALERT_COOLDOWN_MS   3000


/* ============================================================
 * GLOBALS
 * ============================================================ */

static const char *TAG = "POTHOLE_IOT";


/* ============================================================
 * EMBEDDED AWS CERTIFICATES
 * ============================================================ */

extern const uint8_t aws_root_ca_pem_start[]
    asm("_binary_aws_root_ca_pem_start");

extern const uint8_t aws_root_ca_pem_end[]
    asm("_binary_aws_root_ca_pem_end");


extern const uint8_t certificate_pem_crt_start[]
    asm("_binary_certificate_pem_crt_start");

extern const uint8_t certificate_pem_crt_end[]
    asm("_binary_certificate_pem_crt_end");


extern const uint8_t private_pem_key_start[]
    asm("_binary_private_pem_key_start");

extern const uint8_t private_pem_key_end[]
    asm("_binary_private_pem_key_end");


/* ============================================================
 * WIFI STATE
 * ============================================================ */

static EventGroupHandle_t wifi_event_group;

#define WIFI_CONNECTED_BIT BIT0


/* ============================================================
 * MQTT STATE
 * ============================================================ */

static esp_mqtt_client_handle_t mqtt_client = NULL;

static volatile bool mqtt_connected = false;


/* ============================================================
 * I2C STATE
 * ============================================================ */

static i2c_master_bus_handle_t bus_handle;

static i2c_master_dev_handle_t adxl_handle;


/* ============================================================
 * WIFI EVENT HANDLER
 * ============================================================ */

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    (void)arg;
    (void)event_data;


    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }


    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGW(
            TAG,
            "Wi-Fi disconnected. Reconnecting..."
        );

        mqtt_connected = false;

        esp_wifi_connect();

        xEventGroupClearBits(
            wifi_event_group,
            WIFI_CONNECTED_BIT
        );
    }


    else if (event_base == IP_EVENT &&
             event_id == IP_EVENT_STA_GOT_IP)
    {
        xEventGroupSetBits(
            wifi_event_group,
            WIFI_CONNECTED_BIT
        );
    }
}


/* ============================================================
 * WIFI INITIALIZATION
 * ============================================================ */

static void wifi_init_sta(void)
{
    wifi_event_group = xEventGroupCreate();


    ESP_ERROR_CHECK(
        esp_netif_init()
    );


    ESP_ERROR_CHECK(
        esp_event_loop_create_default()
    );


    esp_netif_create_default_wifi_sta();


    wifi_init_config_t cfg =
        WIFI_INIT_CONFIG_DEFAULT();


    ESP_ERROR_CHECK(
        esp_wifi_init(&cfg)
    );


    esp_event_handler_instance_t instance_any_id;

    esp_event_handler_instance_t instance_got_ip;


    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &wifi_event_handler,
            NULL,
            &instance_any_id
        )
    );


    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &wifi_event_handler,
            NULL,
            &instance_got_ip
        )
    );


    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };


    ESP_ERROR_CHECK(
        esp_wifi_set_mode(WIFI_MODE_STA)
    );


    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_STA,
            &wifi_config
        )
    );


    ESP_ERROR_CHECK(
        esp_wifi_start()
    );


    ESP_LOGI(
        TAG,
        "Connecting to Wi-Fi SSID: %s",
        WIFI_SSID
    );


    xEventGroupWaitBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT,
        pdFALSE,
        pdTRUE,
        portMAX_DELAY
    );


    ESP_LOGI(
        TAG,
        "Connected to Wi-Fi"
    );
}


/* ============================================================
 * MQTT EVENT HANDLER
 * ============================================================ */

static void mqtt_event_handler(
    void *handler_args,
    esp_event_base_t base,
    int32_t event_id,
    void *event_data
)
{
    (void)handler_args;
    (void)base;
    (void)event_data;


    switch ((esp_mqtt_event_id_t)event_id)
    {
        case MQTT_EVENT_CONNECTED:

            ESP_LOGI(
                TAG,
                "MQTT Connected to AWS IoT!"
            );

            mqtt_connected = true;

            break;


        case MQTT_EVENT_DISCONNECTED:

            ESP_LOGW(
                TAG,
                "MQTT Disconnected"
            );

            mqtt_connected = false;

            break;


        default:

            break;
    }
}


/* ============================================================
 * AWS IOT INITIALIZATION
 * ============================================================ */

static void aws_iot_init(void)
{
    const esp_mqtt_client_config_t mqtt_cfg = {

        .broker = {
            .address.uri =
                "mqtts://" AWS_IOT_ENDPOINT ":8883",

            .verification.certificate =
                (const char *)aws_root_ca_pem_start,
        },


        .credentials = {
            .authentication = {

                .certificate =
                    (const char *)certificate_pem_crt_start,

                .key =
                    (const char *)private_pem_key_start,
            },
        },
    };


    mqtt_client =
        esp_mqtt_client_init(&mqtt_cfg);


    if (mqtt_client == NULL)
    {
        ESP_LOGE(
            TAG,
            "Failed to initialize MQTT client"
        );

        return;
    }


    ESP_ERROR_CHECK(
        esp_mqtt_client_register_event(
            mqtt_client,
            ESP_EVENT_ANY_ID,
            mqtt_event_handler,
            NULL
        )
    );


    ESP_ERROR_CHECK(
        esp_mqtt_client_start(mqtt_client)
    );
}


/* ============================================================
 * ADXL345 WRITE REGISTER
 * ============================================================ */

static esp_err_t adxl_write_reg(
    uint8_t reg,
    uint8_t value
)
{
    uint8_t data[2] = {
        reg,
        value
    };


    return i2c_master_transmit(
        adxl_handle,
        data,
        sizeof(data),
        1000
    );
}


/* ============================================================
 * ADXL345 READ REGISTERS
 * ============================================================ */

static esp_err_t adxl_read_regs(
    uint8_t reg,
    uint8_t *data,
    size_t len
)
{
    return i2c_master_transmit_receive(
        adxl_handle,
        &reg,
        1,
        data,
        len,
        1000
    );
}


/* ============================================================
 * ADXL345 INITIALIZATION
 * ============================================================ */

static esp_err_t adxl_init(void)
{
    uint8_t devid = 0;


    esp_err_t ret =
        adxl_read_regs(
            REG_DEVID,
            &devid,
            1
        );


    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to read ADXL345 DEVID: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }


    ESP_LOGI(
        TAG,
        "ADXL345 DEVID = 0x%02X",
        devid
    );


    if (devid != EXPECTED_DEVID)
    {
        ESP_LOGE(
            TAG,
            "ADXL345 not detected"
        );

        return ESP_FAIL;
    }


    /*
     * Full resolution
     * +/-4g
     *
     * 0x09:
     *
     * bit 3 = FULL_RES
     * bits 1:0 = 01 -> +/-4g
     */

    ESP_ERROR_CHECK(
        adxl_write_reg(
            REG_DATA_FORMAT,
            0x09
        )
    );


    /*
     * 100 Hz output data rate.
     */

    ESP_ERROR_CHECK(
        adxl_write_reg(
            REG_BW_RATE,
            0x0A
        )
    );


    /*
     * Measurement mode.
     */

    ESP_ERROR_CHECK(
        adxl_write_reg(
            REG_POWER_CTL,
            0x08
        )
    );


    ESP_LOGI(
        TAG,
        "ADXL345 initialized"
    );


    return ESP_OK;
}


/* ============================================================
 * READ ADXL345 ACCELERATION
 * ============================================================ */

static esp_err_t adxl_read_accel(
    float *x,
    float *y,
    float *z
)
{
    uint8_t data[6];


    esp_err_t ret =
        adxl_read_regs(
            REG_DATAX0,
            data,
            sizeof(data)
        );


    if (ret != ESP_OK)
    {
        return ret;
    }


    int16_t raw_x =
        (int16_t)(
            ((uint16_t)data[1] << 8) |
            data[0]
        );


    int16_t raw_y =
        (int16_t)(
            ((uint16_t)data[3] << 8) |
            data[2]
        );


    int16_t raw_z =
        (int16_t)(
            ((uint16_t)data[5] << 8) |
            data[4]
        );


    /*
     * ADXL345 full-resolution sensitivity:
     *
     * approximately 3.9 mg/LSB.
     */

    *x = raw_x * 0.0039f;

    *y = raw_y * 0.0039f;

    *z = raw_z * 0.0039f;


    return ESP_OK;
}


/* ============================================================
 * I2C INITIALIZATION
 * ============================================================ */

static void i2c_init(void)
{
    i2c_master_bus_config_t bus_config = {

        .i2c_port = I2C_PORT,

        .sda_io_num = I2C_SDA,

        .scl_io_num = I2C_SCL,

        .clk_source = I2C_CLK_SRC_DEFAULT,

        .glitch_ignore_cnt = 7,

        .flags.enable_internal_pullup = true
    };


    ESP_ERROR_CHECK(
        i2c_new_master_bus(
            &bus_config,
            &bus_handle
        )
    );


    i2c_device_config_t device_config = {

        .dev_addr_length =
            I2C_ADDR_BIT_LEN_7,

        .device_address =
            ADXL345_ADDR,

        .scl_speed_hz =
            400000
    };


    ESP_ERROR_CHECK(
        i2c_master_bus_add_device(
            bus_handle,
            &device_config,
            &adxl_handle
        )
    );
}


/* ============================================================
 * FEATURE EXTRACTION
 *
 * MUST MATCH THE TRAINING FEATURES
 *
 * 0   x_mean
 * 1   x_std
 * 2   x_min
 * 3   x_max
 *
 * 4   y_mean
 * 5   y_std
 * 6   y_min
 * 7   y_max
 *
 * 8   z_mean
 * 9   z_std
 * 10  z_min
 * 11  z_max
 *
 * 12  mag_mean
 * 13  mag_std
 * 14  mag_min
 * 15  mag_max
 *
 * 16  mag_rms
 * 17  mag_range
 * ============================================================ */

static void extract_features(
    const float *x,
    const float *y,
    const float *z,
    float *f
)
{
    float magnitude[WINDOW_SAMPLES];


    float x_sum = 0.0f;
    float y_sum = 0.0f;
    float z_sum = 0.0f;

    float mag_sum = 0.0f;
    float mag_sq_sum = 0.0f;


    /* --------------------------------------------------------
     * Magnitude and sums
     * -------------------------------------------------------- */

    for (int i = 0;
         i < WINDOW_SAMPLES;
         i++)
    {
        magnitude[i] =
            sqrtf(
                x[i] * x[i] +
                y[i] * y[i] +
                z[i] * z[i]
            );


        x_sum += x[i];

        y_sum += y[i];

        z_sum += z[i];

        mag_sum += magnitude[i];

        mag_sq_sum +=
            magnitude[i] *
            magnitude[i];
    }


    /* --------------------------------------------------------
     * Means
     * -------------------------------------------------------- */

    float x_mean =
        x_sum / WINDOW_SAMPLES;

    float y_mean =
        y_sum / WINDOW_SAMPLES;

    float z_mean =
        z_sum / WINDOW_SAMPLES;

    float mag_mean =
        mag_sum / WINDOW_SAMPLES;


    /* --------------------------------------------------------
     * Min/max initialization
     * -------------------------------------------------------- */

    float x_min = x[0];
    float x_max = x[0];

    float y_min = y[0];
    float y_max = y[0];

    float z_min = z[0];
    float z_max = z[0];

    float mag_min = magnitude[0];
    float mag_max = magnitude[0];


    /* --------------------------------------------------------
     * Min/max
     * -------------------------------------------------------- */

    for (int i = 1;
         i < WINDOW_SAMPLES;
         i++)
    {
        if (x[i] < x_min)
        {
            x_min = x[i];
        }

        if (x[i] > x_max)
        {
            x_max = x[i];
        }


        if (y[i] < y_min)
        {
            y_min = y[i];
        }

        if (y[i] > y_max)
        {
            y_max = y[i];
        }


        if (z[i] < z_min)
        {
            z_min = z[i];
        }

        if (z[i] > z_max)
        {
            z_max = z[i];
        }


        if (magnitude[i] < mag_min)
        {
            mag_min = magnitude[i];
        }

        if (magnitude[i] > mag_max)
        {
            mag_max = magnitude[i];
        }
    }


    /* --------------------------------------------------------
     * Population variance
     *
     * This matches numpy.std()
     * -------------------------------------------------------- */

    float x_var = 0.0f;
    float y_var = 0.0f;
    float z_var = 0.0f;
    float mag_var = 0.0f;


    for (int i = 0;
         i < WINDOW_SAMPLES;
         i++)
    {
        float dx =
            x[i] - x_mean;

        float dy =
            y[i] - y_mean;

        float dz =
            z[i] - z_mean;

        float dm =
            magnitude[i] - mag_mean;


        x_var += dx * dx;

        y_var += dy * dy;

        z_var += dz * dz;

        mag_var += dm * dm;
    }


    /* --------------------------------------------------------
     * Standard deviations
     * -------------------------------------------------------- */

    float x_std =
        sqrtf(
            x_var / WINDOW_SAMPLES
        );


    float y_std =
        sqrtf(
            y_var / WINDOW_SAMPLES
        );


    float z_std =
        sqrtf(
            z_var / WINDOW_SAMPLES
        );


    float mag_std =
        sqrtf(
            mag_var / WINDOW_SAMPLES
        );


    /* --------------------------------------------------------
     * Magnitude RMS
     * -------------------------------------------------------- */

    float mag_rms =
        sqrtf(
            mag_sq_sum /
            WINDOW_SAMPLES
        );


    /* --------------------------------------------------------
     * Final feature vector
     * -------------------------------------------------------- */

    f[0]  = x_mean;
    f[1]  = x_std;
    f[2]  = x_min;
    f[3]  = x_max;

    f[4]  = y_mean;
    f[5]  = y_std;
    f[6]  = y_min;
    f[7]  = y_max;

    f[8]  = z_mean;
    f[9]  = z_std;
    f[10] = z_min;
    f[11] = z_max;

    f[12] = mag_mean;
    f[13] = mag_std;
    f[14] = mag_min;
    f[15] = mag_max;

    f[16] = mag_rms;
    f[17] = mag_max - mag_min;
}


/* ============================================================
 * PRINT FEATURES
 * ============================================================ */

static void print_features(
    const float *f
)
{
    printf("\n");

    printf(
        "FEATURES\n"
    );

    printf(
        "----------------------------------------\n"
    );


    printf(
        "x_mean    = %.4f\n",
        f[0]
    );

    printf(
        "x_std     = %.4f\n",
        f[1]
    );

    printf(
        "x_min     = %.4f\n",
        f[2]
    );

    printf(
        "x_max     = %.4f\n",
        f[3]
    );


    printf(
        "y_mean    = %.4f\n",
        f[4]
    );

    printf(
        "y_std     = %.4f\n",
        f[5]
    );

    printf(
        "y_min     = %.4f\n",
        f[6]
    );

    printf(
        "y_max     = %.4f\n",
        f[7]
    );


    printf(
        "z_mean    = %.4f\n",
        f[8]
    );

    printf(
        "z_std     = %.4f\n",
        f[9]
    );

    printf(
        "z_min     = %.4f\n",
        f[10]
    );

    printf(
        "z_max     = %.4f\n",
        f[11]
    );


    printf(
        "mag_mean  = %.4f\n",
        f[12]
    );

    printf(
        "mag_std   = %.4f\n",
        f[13]
    );

    printf(
        "mag_min   = %.4f\n",
        f[14]
    );

    printf(
        "mag_max   = %.4f\n",
        f[15]
    );


    printf(
        "mag_rms   = %.4f\n",
        f[16]
    );

    printf(
        "mag_range = %.4f\n",
        f[17]
    );


    printf(
        "----------------------------------------\n"
    );
}


/* ============================================================
 * CLASS NAME
 * ============================================================ */

static const char *class_name(
    int result
)
{
    switch (result)
    {
        case CLASS_NORMAL:
            return "NORMAL";

        case CLASS_SPEED_HUMP:
            return "SPEED HUMP";

        case CLASS_POTHOLE:
            return "POTHOLE";

        default:
            return "UNKNOWN";
    }
}


/* ============================================================
 * PUBLISH POTHOLE ALERT
 * ============================================================ */

static void publish_pothole_alert(void)
{
    /*
     * These are currently mock coordinates.
     *
     * GPS can be integrated later.
     */

    float latitude =
        12.9716f;

    float longitude =
        79.1588f;


    char payload[160];


    snprintf(
        payload,
        sizeof(payload),
        "{\"event\":\"POTHOLE\",\"coordinates\":[%.4f,%.4f]}",
        latitude,
        longitude
    );


    if (!mqtt_connected)
    {
        ESP_LOGW(
            TAG,
            "Pothole detected, but MQTT is not connected"
        );

        return;
    }


    int msg_id =
        esp_mqtt_client_publish(
            mqtt_client,
            AWS_IOT_TOPIC,
            payload,
            0,
            1,
            0
        );


    if (msg_id < 0)
    {
        ESP_LOGE(
            TAG,
            "Failed to publish pothole alert"
        );

        return;
    }


    ESP_LOGI(
        TAG,
        "Published alert (msg_id=%d): %s",
        msg_id,
        payload
    );
}


/* ============================================================
 * MAIN
 * ============================================================ */

void app_main(void)
{
    printf("\n");

    printf(
        "========================================\n"
    );

    printf(
        "       ESP32 POTHOLE IOT SYSTEM\n"
    );

    printf(
        "========================================\n"
    );

    printf("\n");


    /* --------------------------------------------------------
     * Initialize NVS
     * -------------------------------------------------------- */

    esp_err_t ret =
        nvs_flash_init();


    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(
            nvs_flash_erase()
        );


        ret =
            nvs_flash_init();
    }


    ESP_ERROR_CHECK(ret);


    /* --------------------------------------------------------
     * Wi-Fi
     * -------------------------------------------------------- */

    wifi_init_sta();


    /* --------------------------------------------------------
     * AWS IoT MQTT
     * -------------------------------------------------------- */

    aws_iot_init();


    /* --------------------------------------------------------
     * ADXL345
     * -------------------------------------------------------- */

    i2c_init();


    if (adxl_init() != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "ADXL345 initialization failed"
        );

        return;
    }


    /* --------------------------------------------------------
     * Buffers
     * -------------------------------------------------------- */

    float x[WINDOW_SAMPLES];

    float y[WINDOW_SAMPLES];

    float z[WINDOW_SAMPLES];

    float features[NUM_FEATURES];


    /* --------------------------------------------------------
     * Sampling schedule
     * -------------------------------------------------------- */

    int64_t next_sample =
        esp_timer_get_time();


    /* --------------------------------------------------------
     * Main inference loop
     * -------------------------------------------------------- */

    while (1)
    {
        printf("\n");

        printf(
            "========================================\n"
        );

        printf(
            "       READY FOR NEXT WINDOW\n"
        );

        printf(
            "========================================\n"
        );

        printf("\n");


        printf(
            "Keep the sensor stationary for NORMAL,\n"
            "or physically perform the event you want\n"
            "to classify.\n"
        );

        printf("\n");


        /* ----------------------------------------------------
         * Countdown
         * ---------------------------------------------------- */

        printf(
            "Starting in 3...\n"
        );

        vTaskDelay(
            pdMS_TO_TICKS(1000)
        );


        printf(
            "2...\n"
        );

        vTaskDelay(
            pdMS_TO_TICKS(1000)
        );


        printf(
            "1...\n"
        );

        vTaskDelay(
            pdMS_TO_TICKS(1000)
        );


        printf("\n");

        printf(
            ">>> RECORDING 1 SECOND <<<\n"
        );


        /* ----------------------------------------------------
         * Collect exactly 100 samples at 100 Hz
         * ---------------------------------------------------- */

        for (int i = 0;
             i < WINDOW_SAMPLES;
             i++)
        {
            esp_err_t read_ret =
                adxl_read_accel(
                    &x[i],
                    &y[i],
                    &z[i]
                );


            if (read_ret != ESP_OK)
            {
                ESP_LOGW(
                    TAG,
                    "ADXL345 read failed at sample %d: %s",
                    i,
                    esp_err_to_name(read_ret)
                );


                /*
                 * Keep the same fallback behavior
                 * as the previous IoT implementation.
                 */

                x[i] = 0.0f;
                y[i] = 0.0f;
                z[i] = 0.0f;
            }


            next_sample +=
                SAMPLE_PERIOD_MS * 1000;


            int64_t delay_us =
                next_sample -
                esp_timer_get_time();


            if (delay_us > 0)
            {
                vTaskDelay(
                    pdMS_TO_TICKS(
                        delay_us / 1000
                    )
                );
            }
            else
            {
                next_sample =
                    esp_timer_get_time();
            }
        }


        printf("\n");

        printf(
            ">>> WINDOW COMPLETE <<<\n"
        );


        /* ----------------------------------------------------
         * Feature extraction
         * ---------------------------------------------------- */

        extract_features(
            x,
            y,
            z,
            features
        );


        /* ----------------------------------------------------
         * Print features
         * ---------------------------------------------------- */

        print_features(
            features
        );


        /* ----------------------------------------------------
         * ML inference
         * ---------------------------------------------------- */

        int result =
            pothole_predict(
                features
            );


        /* ----------------------------------------------------
         * Print classification
         * ---------------------------------------------------- */

        printf("\n");

        printf(
            "========================================\n"
        );

        printf(
            "        ESP32 CLASSIFICATION\n"
        );

        printf(
            "========================================\n"
        );


        printf(
            "RESULT: %s\n",
            class_name(result)
        );


        printf(
            "========================================\n"
        );


        /* ----------------------------------------------------
         * AWS alert
         * ---------------------------------------------------- */

        if (result == CLASS_POTHOLE)
        {
            ESP_LOGW(
                TAG,
                "POTHOLE DETECTED!"
            );


            publish_pothole_alert();


            /*
             * Prevent immediate repeated alerts.
             */

            vTaskDelay(
                pdMS_TO_TICKS(
                    ALERT_COOLDOWN_MS
                )
            );
        }
    }
}
