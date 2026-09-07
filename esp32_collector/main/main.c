#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2c_master.h"

#include "esp_err.h"
#include "esp_timer.h"


/* ============================================================
 * ADXL345 CONFIGURATION
 * ============================================================ */

#define I2C_PORT            I2C_NUM_0

#define I2C_SDA             21
#define I2C_SCL             22

#define ADXL345_ADDR        0x53

#define REG_DEVID           0x00
#define REG_BW_RATE         0x2C
#define REG_POWER_CTL       0x2D
#define REG_DATA_FORMAT     0x31
#define REG_DATAX0          0x32

#define EXPECTED_DEVID      0xE5

#define SAMPLE_RATE_HZ      100
#define SAMPLE_PERIOD_MS    10


/* ============================================================
 * GLOBAL I2C HANDLES
 * ============================================================ */

static i2c_master_bus_handle_t bus_handle;
static i2c_master_dev_handle_t adxl_handle;


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

    /* Read device ID */

    esp_err_t ret = adxl_read_regs(
        REG_DEVID,
        &devid,
        1
    );

    if (ret != ESP_OK)
    {
        printf(
            "ERROR: Failed to read ADXL345 DEVID: %s\n",
            esp_err_to_name(ret)
        );

        return ret;
    }

    printf(
        "ADXL345 DEVID = 0x%02X\n",
        devid
    );

    if (devid != EXPECTED_DEVID)
    {
        printf(
            "ERROR: ADXL345 not detected.\n"
        );

        return ESP_FAIL;
    }


    /*
     * DATA_FORMAT = 0x09
     *
     * Bit 3 = FULL_RES = 1
     * Bits 1:0 = 01 -> +/-4g
     */

    ESP_ERROR_CHECK(
        adxl_write_reg(
            REG_DATA_FORMAT,
            0x09
        )
    );


    /*
     * BW_RATE = 0x0A
     *
     * 100 Hz output data rate
     */

    ESP_ERROR_CHECK(
        adxl_write_reg(
            REG_BW_RATE,
            0x0A
        )
    );


    /*
     * POWER_CTL = 0x08
     *
     * Measurement mode
     */

    ESP_ERROR_CHECK(
        adxl_write_reg(
            REG_POWER_CTL,
            0x08
        )
    );


    printf(
        "ADXL345 initialized.\n"
    );

    return ESP_OK;
}


/* ============================================================
 * READ ACCELERATION
 * ============================================================ */

static esp_err_t adxl_read_accel(
    float *x,
    float *y,
    float *z
)
{
    uint8_t data[6];

    esp_err_t ret = adxl_read_regs(
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
     * ADXL345 full-resolution scale:
     *
     * approximately 3.9 mg/LSB
     *
     * Output is therefore in g.
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
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ADXL345_ADDR,
        .scl_speed_hz = 400000
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
 * MAIN
 *
 * IMPORTANT:
 *
 * THIS FIRMWARE DOES NOT PERFORM ML.
 *
 * It only streams:
 *
 * timestamp,x,y,z
 *
 * to the laptop.
 * ============================================================ */

void app_main(void)
{
    printf("\n");
    printf("========================================\n");
    printf("     ESP32 POTHOLE ML DATA COLLECTOR\n");
    printf("========================================\n");
    printf("\n");

    printf(
        "I2C SDA    : GPIO%d\n",
        I2C_SDA
    );

    printf(
        "I2C SCL    : GPIO%d\n",
        I2C_SCL
    );

    printf(
        "I2C address : 0x%02X\n",
        ADXL345_ADDR
    );

    printf(
        "Sample rate : %d Hz\n",
        SAMPLE_RATE_HZ
    );

    printf("\n");


    /* Initialize I2C */

    i2c_init();


    /* Initialize ADXL345 */

    if (adxl_init() != ESP_OK)
    {
        printf(
            "ADXL345 initialization failed.\n"
        );

        return;
    }


    printf("\n");
    printf("========================================\n");
    printf("           SENSOR READY\n");
    printf("========================================\n");
    printf("\n");

    printf(
        "Streaming acceleration data...\n"
    );

    printf(
        "Laptop Python program controls recording.\n"
    );

    printf("\n");


    /*
     * This marker tells collect.py that
     * the sensor stream has started.
     */

    printf("DATA_BEGIN\n");

    printf(
        "timestamp,x,y,z\n"
    );


    /*
     * Timing reference.
     *
     * esp_timer_get_time() returns microseconds.
     */

    int64_t next_sample =
        esp_timer_get_time();


    while (1)
    {
        float x;
        float y;
        float z;


        /* Read ADXL345 */

        esp_err_t ret =
            adxl_read_accel(
                &x,
                &y,
                &z
            );


        if (ret == ESP_OK)
        {
            int64_t now =
                esp_timer_get_time();


            /*
             * Timestamp in milliseconds.
             */

            printf(
                "%lld,%.5f,%.5f,%.5f\n",
                now / 1000,
                x,
                y,
                z
            );
        }


        /*
         * Schedule next sample.
         */

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
            /*
             * We fell behind.
             * Restart timing reference.
             */

            next_sample =
                esp_timer_get_time();
        }
    }
}
