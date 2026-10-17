/*
 * MPU-6050 (GY-521) backend: accelerometer only, 100 Hz, +/-8 g
 * (4096 LSB/g), 44 Hz digital low-pass. The gyro is not used.
 *
 * Why no hardware FIFO: the MPU-6050 FIFO is a plain byte stream. If one
 * I2C burst is cut short (a breadboard wire flickering while the board is
 * shaken), a partial sample is popped and every later sample is read
 * misaligned - permanently scrambled data. Instead a small task polls the
 * DATA_RDY flag every 1 ms and reads each sample as one 6-byte register
 * snapshot, so a glitch can damage at most a single sample. Timing still
 * comes from the sensor's own 100 Hz clock.
 *
 * Also accepts MPU-6500 / MPU-9250 style clones that answer with a
 * different WHO_AM_I but share this register map.
 */

#include "accel_backend.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"

#define REG_SMPLRT_DIV      0x19
#define REG_CONFIG          0x1A
#define REG_ACCEL_CONFIG    0x1C
#define REG_INT_STATUS      0x3A
#define REG_ACCEL_XOUT_H    0x3B
#define REG_PWR_MGMT_1      0x6B
#define REG_WHO_AM_I        0x75

#define LSB_G               (1.0f / 4096.0f)
#define QUEUE_LEN           64          /* 640 ms of samples */

static const char *TAG = "MPU6050";

static i2c_master_dev_handle_t dev;
static QueueHandle_t samples;
static volatile int lost;


static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    uint8_t data[2] = { reg, value };

    return i2c_master_transmit(dev, data, sizeof(data), 100);
}


static esp_err_t read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, data, len, 100);
}


static void poll_task(void *arg)
{
    while (1)
    {
        uint8_t status = 0;
        uint8_t d[6];

        /* reading INT_STATUS clears DATA_RDY */
        if (read_regs(REG_INT_STATUS, &status, 1) == ESP_OK && (status & 0x01) &&
            read_regs(REG_ACCEL_XOUT_H, d, sizeof(d)) == ESP_OK)
        {
            accel_sample_t s = {
                .x = (int16_t)((d[0] << 8) | d[1]) * LSB_G,
                .y = (int16_t)((d[2] << 8) | d[3]) * LSB_G,
                .z = (int16_t)((d[4] << 8) | d[5]) * LSB_G,
            };

            if (xQueueSend(samples, &s, 0) != pdTRUE)
            {
                lost = 1;
            }
        }

        vTaskDelay(1);      /* 1 ms at CONFIG_FREERTOS_HZ=1000 */
    }
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

    if (ret == ESP_OK && samples == NULL)
    {
        samples = xQueueCreate(QUEUE_LEN, sizeof(accel_sample_t));
        xTaskCreatePinnedToCore(poll_task, "mpu6050", 2560, NULL, 12, NULL, 1);
    }

    return ret;
}


static int mpu6050_read(accel_sample_t *out, int max, int *overflow)
{
    int n = 0;

    while (n < max && xQueueReceive(samples, &out[n], 0) == pdTRUE)
    {
        n++;
    }

    if (overflow && lost)
    {
        *overflow = 1;
        lost = 0;
    }

    return n;
}


const accel_backend_t mpu6050_backend = {
    .name = "MPU-6050",
    .init = mpu6050_init,
    .read = mpu6050_read,
};
