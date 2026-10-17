/*
 * ADXL345 backend: 100 Hz, +/-8 g full resolution (3.9 mg/LSB),
 * FIFO stream mode (holds the newest 32 samples).
 */

#include "accel_backend.h"

#include "esp_log.h"

#define REG_DEVID           0x00
#define REG_BW_RATE         0x2C
#define REG_POWER_CTL       0x2D
#define REG_DATA_FORMAT     0x31
#define REG_DATAX0          0x32
#define REG_FIFO_CTL        0x38
#define REG_FIFO_STATUS     0x39

#define EXPECTED_DEVID      0xE5
#define FIFO_DEPTH          32
#define LSB_G               0.0039f

static const char *TAG = "ADXL345";

static i2c_master_dev_handle_t dev;


static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    uint8_t data[2] = { reg, value };

    return i2c_master_transmit(dev, data, sizeof(data), 100);
}


static esp_err_t read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, data, len, 100);
}


static esp_err_t adxl345_init(i2c_master_bus_handle_t bus)
{
    /* 0x53 with SDO->GND (expected), 0x1D with SDO high/floating */
    const uint16_t addresses[] = { 0x53, 0x1D };
    uint16_t addr = 0;

    for (size_t i = 0; i < 2 && addr == 0; i++)
    {
        if (i2c_master_probe(bus, addresses[i], 20) == ESP_OK)
        {
            addr = addresses[i];
        }
    }

    if (addr == 0)
    {
        return ESP_ERR_NOT_FOUND;
    }

    if (dev == NULL)
    {
        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr,
            .scl_speed_hz = 400000,
        };

        esp_err_t ret = i2c_master_bus_add_device(bus, &cfg, &dev);
        if (ret != ESP_OK)
        {
            return ret;
        }
    }

    uint8_t devid = 0;
    esp_err_t ret = read_regs(REG_DEVID, &devid, 1);

    if (ret != ESP_OK)
    {
        return ret;
    }

    if (devid != EXPECTED_DEVID)
    {
        /* 0x1D/0x53 can be other chips; let the next backend try */
        ESP_LOGW(TAG, "device at 0x%02X has DEVID 0x%02X, not an ADXL345", addr, devid);
        i2c_master_bus_rm_device(dev);
        dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    if (addr == 0x1D)
    {
        ESP_LOGW(TAG, "found at 0x1D: SDO is not tied to GND (works, but wire SDO->GND)");
    }

    /* standby while configuring */
    ret = write_reg(REG_POWER_CTL, 0x00);

    /* FULL_RES (bit 3) | range +/-8 g (0b10): 3.9 mg/LSB, headroom for impacts */
    if (ret == ESP_OK) ret = write_reg(REG_DATA_FORMAT, 0x0A);

    /* 100 Hz output data rate (50 Hz bandwidth) */
    if (ret == ESP_OK) ret = write_reg(REG_BW_RATE, 0x0A);

    /* FIFO stream mode */
    if (ret == ESP_OK) ret = write_reg(REG_FIFO_CTL, 0x80 | 16);

    /* measurement mode */
    if (ret == ESP_OK) ret = write_reg(REG_POWER_CTL, 0x08);

    return ret;
}


static int adxl345_read(accel_sample_t *out, int max, int *overflow)
{
    uint8_t status = 0;

    if (read_regs(REG_FIFO_STATUS, &status, 1) != ESP_OK)
    {
        return -1;
    }

    int entries = status & 0x3F;

    if (overflow)
    {
        *overflow = (entries >= FIFO_DEPTH);
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

        out[i].x = (int16_t)((d[1] << 8) | d[0]) * LSB_G;
        out[i].y = (int16_t)((d[3] << 8) | d[2]) * LSB_G;
        out[i].z = (int16_t)((d[5] << 8) | d[4]) * LSB_G;
    }

    return entries;
}


const accel_backend_t adxl345_backend = {
    .name = "ADXL345",
    .init = adxl345_init,
    .read = adxl345_read,
};
