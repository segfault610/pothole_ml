/*
 * MPU-6050 (GY-521) backend: accelerometer only, 100 Hz, +/-8 g
 * (4096 LSB/g), 44 Hz digital low-pass, samples buffered in the
 * 1024-byte FIFO (170 samples, 1.7 s). The gyro is not used.
 *
 * Also accepts the common MPU-6500 / MPU-9250 clones that answer with a
 * different WHO_AM_I but share this register map.
 */

#include "accel_backend.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define REG_SMPLRT_DIV      0x19
#define REG_CONFIG          0x1A
#define REG_ACCEL_CONFIG    0x1C
#define REG_FIFO_EN         0x23
#define REG_INT_STATUS      0x3A
#define REG_USER_CTRL       0x6A
#define REG_PWR_MGMT_1      0x6B
#define REG_FIFO_COUNT_H    0x72
#define REG_FIFO_R_W        0x74
#define REG_WHO_AM_I        0x75

#define LSB_G               (1.0f / 4096.0f)
#define BYTES_PER_SAMPLE    6

static const char *TAG = "MPU6050";

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


static esp_err_t fifo_reset(void)
{
    /* disable FIFO, reset it, enable it again */
    esp_err_t ret = write_reg(REG_USER_CTRL, 0x00);
    if (ret == ESP_OK) ret = write_reg(REG_USER_CTRL, 0x04);
    if (ret == ESP_OK) ret = write_reg(REG_USER_CTRL, 0x40);
    return ret;
}


static esp_err_t mpu6050_init(i2c_master_bus_handle_t bus)
{
    /* 0x68 with AD0->GND (expected), 0x69 with AD0 high */
    const uint16_t addresses[] = { 0x68, 0x69 };
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

    uint8_t who = 0;
    esp_err_t ret = read_regs(REG_WHO_AM_I, &who, 1);

    if (ret != ESP_OK)
    {
        return ret;
    }

    ESP_LOGI(TAG, "at 0x%02X, WHO_AM_I=0x%02X%s", addr, who,
             who == 0x68 ? "" : " (clone/variant, using MPU-6050 registers)");

    /* reset, then wake up with the X-gyro PLL as clock (more stable) */
    ret = write_reg(REG_PWR_MGMT_1, 0x80);
    vTaskDelay(pdMS_TO_TICKS(100));
    if (ret == ESP_OK) ret = write_reg(REG_PWR_MGMT_1, 0x01);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* DLPF_CFG=3: accel 44 Hz bandwidth, internal rate 1 kHz */
    if (ret == ESP_OK) ret = write_reg(REG_CONFIG, 0x03);

    /* 1 kHz / (1 + 9) = 100 Hz */
    if (ret == ESP_OK) ret = write_reg(REG_SMPLRT_DIV, 9);

    /* AFS_SEL=2: +/-8 g */
    if (ret == ESP_OK) ret = write_reg(REG_ACCEL_CONFIG, 0x10);

    /* only the accelerometer goes into the FIFO */
    if (ret == ESP_OK) ret = write_reg(REG_FIFO_EN, 0x08);
    if (ret == ESP_OK) ret = fifo_reset();

    return ret;
}


static int mpu6050_read(accel_sample_t *out, int max, int *overflow)
{
    uint8_t status = 0;
    uint8_t cnt[2];

    if (read_regs(REG_INT_STATUS, &status, 1) != ESP_OK ||
        read_regs(REG_FIFO_COUNT_H, cnt, 2) != ESP_OK)
    {
        return -1;
    }

    if (status & 0x10)
    {
        /* FIFO overflowed: its contents are no longer sample-aligned */
        if (overflow)
        {
            *overflow = 1;
        }

        fifo_reset();
        return 0;
    }

    int n = ((cnt[0] << 8) | cnt[1]) / BYTES_PER_SAMPLE;

    if (n > max)
    {
        n = max;
    }

    if (n == 0)
    {
        return 0;
    }

    uint8_t d[ACCEL_MAX_BATCH * BYTES_PER_SAMPLE];

    if (n > ACCEL_MAX_BATCH)
    {
        n = ACCEL_MAX_BATCH;
    }

    if (read_regs(REG_FIFO_R_W, d, n * BYTES_PER_SAMPLE) != ESP_OK)
    {
        return -1;
    }

    for (int i = 0; i < n; i++)
    {
        const uint8_t *s = &d[i * BYTES_PER_SAMPLE];

        /* big-endian, unlike the ADXL345 */
        out[i].x = (int16_t)((s[0] << 8) | s[1]) * LSB_G;
        out[i].y = (int16_t)((s[2] << 8) | s[3]) * LSB_G;
        out[i].z = (int16_t)((s[4] << 8) | s[5]) * LSB_G;
    }

    return n;
}


const accel_backend_t mpu6050_backend = {
    .name = "MPU-6050",
    .init = mpu6050_init,
    .read = mpu6050_read,
};
