#include "adxl345.h"

#include <stdio.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

#define REG_DEVID           0x00
#define REG_BW_RATE         0x2C
#define REG_POWER_CTL       0x2D
#define REG_DATA_FORMAT     0x31
#define REG_DATAX0          0x32
#define REG_FIFO_CTL        0x38
#define REG_FIFO_STATUS     0x39

#define EXPECTED_DEVID      0xE5

static const char *TAG = "ADXL345";

static i2c_master_bus_handle_t bus_handle;
static i2c_master_dev_handle_t dev_handle;


static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    uint8_t data[2] = { reg, value };

    return i2c_master_transmit(dev_handle, data, sizeof(data), 100);
}


static esp_err_t read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(dev_handle, &reg, 1, data, len, 100);
}


/* Wiring diagnostics: which addresses answer, and are the lines stuck? */
static void scan_bus(int sda, int scl)
{
    int found = 0;

    for (int addr = 0x08; addr < 0x78; addr++)
    {
        if (i2c_master_probe(bus_handle, addr, 20) == ESP_OK)
        {
            ESP_LOGW(TAG, "I2C device at 0x%02X%s", addr,
                     addr == 0x1D ? " <- ADXL345 with SDO high/floating: tie SDO to GND" : "");
            found++;
        }
    }

    if (found == 0)
    {
        ESP_LOGW(TAG, "I2C scan: no device. SDA(GPIO%d)=%d SCL(GPIO%d)=%d "
                 "(both should read 1; 0 = shorted/stuck low)",
                 sda, gpio_get_level(sda), scl, gpio_get_level(scl));
    }
}


esp_err_t adxl345_init(int i2c_port, int sda, int scl)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = i2c_port,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t ret;

    /* bus + device are created once; later calls just re-probe the sensor */
    if (bus_handle == NULL)
    {
        ret = i2c_new_master_bus(&bus_config, &bus_handle);
        if (ret != ESP_OK)
        {
            return ret;
        }

        i2c_device_config_t device_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = ADXL345_ADDR,
            .scl_speed_hz = 400000,
        };

        ret = i2c_master_bus_add_device(bus_handle, &device_config, &dev_handle);
        if (ret != ESP_OK)
        {
            return ret;
        }
    }

    uint8_t devid = 0;

    ret = read_regs(REG_DEVID, &devid, 1);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "No answer on I2C (SDA=%d SCL=%d): %s. Check wiring, "
                 "CS->3V3, SDO->GND.", sda, scl, esp_err_to_name(ret));
        scan_bus(sda, scl);
        return ret;
    }

    if (devid != EXPECTED_DEVID)
    {
        ESP_LOGE(TAG, "DEVID 0x%02X, expected 0x%02X", devid, EXPECTED_DEVID);
        return ESP_FAIL;
    }

    /* standby while configuring */
    ret = write_reg(REG_POWER_CTL, 0x00);

    /* FULL_RES (bit 3) | range +/-8 g (0b10): 3.9 mg/LSB, headroom for impacts */
    if (ret == ESP_OK) ret = write_reg(REG_DATA_FORMAT, 0x0A);

    /* 100 Hz output data rate (50 Hz bandwidth) */
    if (ret == ESP_OK) ret = write_reg(REG_BW_RATE, 0x0A);

    /* FIFO stream mode: always holds the newest 32 samples */
    if (ret == ESP_OK) ret = write_reg(REG_FIFO_CTL, 0x80 | 16);

    /* measurement mode */
    if (ret == ESP_OK) ret = write_reg(REG_POWER_CTL, 0x08);

    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "ready: 100 Hz, +/-8 g, FIFO stream");
    }

    return ret;
}


int adxl345_read_fifo(adxl345_raw_t *out, int max, int *overflow)
{
    uint8_t status = 0;

    if (read_regs(REG_FIFO_STATUS, &status, 1) != ESP_OK)
    {
        return -1;
    }

    int entries = status & 0x3F;

    if (overflow)
    {
        *overflow = (entries >= ADXL345_FIFO_DEPTH);
    }

    if (entries > max)
    {
        entries = max;
    }

    for (int i = 0; i < entries; i++)
    {
        uint8_t d[6];

        /* each 6-byte burst read pops one FIFO entry */
        if (read_regs(REG_DATAX0, d, sizeof(d)) != ESP_OK)
        {
            return -1;
        }

        out[i].x = (int16_t)((d[1] << 8) | d[0]);
        out[i].y = (int16_t)((d[3] << 8) | d[2]);
        out[i].z = (int16_t)((d[5] << 8) | d[4]);
    }

    return entries;
}
