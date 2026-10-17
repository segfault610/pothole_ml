#include "accel.h"
#include "accel_backend.h"

#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "ACCEL";

static i2c_master_bus_handle_t bus;
static const accel_backend_t *active;

static const accel_backend_t *const BACKENDS[] = {
    &adxl345_backend,
    &mpu6050_backend,
};


/* Wiring diagnostics: which addresses answer, and are the lines stuck? */
static void scan_bus(int sda, int scl)
{
    int found = 0;

    for (int addr = 0x08; addr < 0x78; addr++)
    {
        if (i2c_master_probe(bus, addr, 20) == ESP_OK)
        {
            ESP_LOGW(TAG, "I2C device at 0x%02X (not a supported accelerometer setup)", addr);
            found++;
        }
    }

    if (found == 0)
    {
        ESP_LOGW(TAG, "I2C scan: no device. SDA(GPIO%d)=%d SCL(GPIO%d)=%d "
                 "(both should read 1; 0 = shorted/stuck low). Check power and wiring.",
                 sda, gpio_get_level(sda), scl, gpio_get_level(scl));
    }
}


esp_err_t accel_init(int i2c_port, int sda, int scl)
{
    if (bus == NULL)
    {
        i2c_master_bus_config_t cfg = {
            .i2c_port = i2c_port,
            .sda_io_num = sda,
            .scl_io_num = scl,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };

        esp_err_t ret = i2c_new_master_bus(&cfg, &bus);
        if (ret != ESP_OK)
        {
            return ret;
        }
    }

    for (size_t i = 0; i < sizeof(BACKENDS) / sizeof(BACKENDS[0]); i++)
    {
        esp_err_t ret = BACKENDS[i]->init(bus);

        if (ret == ESP_OK)
        {
            active = BACKENDS[i];
            ESP_LOGI(TAG, "using %s: %d Hz, +/-8 g", active->name, ACCEL_RATE_HZ);
            return ESP_OK;
        }

        if (ret != ESP_ERR_NOT_FOUND)
        {
            ESP_LOGE(TAG, "%s found but setup failed: %s", BACKENDS[i]->name, esp_err_to_name(ret));
            return ret;
        }
    }

    ESP_LOGE(TAG, "no ADXL345 (0x53/0x1D) or MPU-6050 (0x68/0x69) on SDA=GPIO%d SCL=GPIO%d", sda, scl);
    scan_bus(sda, scl);

    return ESP_ERR_NOT_FOUND;
}


const char *accel_name(void)
{
    return active ? active->name : "none";
}


int accel_read(accel_sample_t *out, int max, int *overflow)
{
    if (overflow)
    {
        *overflow = 0;
    }

    return active ? active->read(out, max, overflow) : -1;
}
