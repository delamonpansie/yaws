/**
 * ESP-IDF driver for ADT7410 Digital Temperature Sensor
 */
#include <esp_idf_lib_helpers.h>
#include <i2cdev.h>
#include "adt7410.h"

#include <math.h>
#include <esp_log.h>

static const char *TAG = "adt7410";

#define I2C_FREQ_HZ 400000 // 400 kHz

#define MANUFACTURER_ID 0b11001

#define REG_T_MSB      0x00  // Power-on default: 0x00
#define REG_T_LSB      0x01  // Power-on default: 0x00
#define REG_STATUS     0x02  // Power-on default: 0x00
#define REG_CONF       0x03  // Power-on default: 0x00
#define REG_T_HIGH_MSB 0x04  // Power-on default: 0x20 (64°C)
#define REG_T_HIGH_LSB 0x05  // Power-on default: 0x00
#define REG_T_LOW_MSB  0x06  // Power-on default: 0x05 (5°C)
#define REG_T_LOW_LSB  0x07  // Power-on default: 0x00
#define REG_T_CRIT_MSB 0x08  // Power-on default: 0x49 (147°C)
#define REG_T_CRIT_LSB 0x09  // Power-on default: 0x80
#define REG_T_HYST     0x0a  // Power-on default: 0x05 (5°C)
#define REG_ID         0x0b  // Power-on default: 0xcx

#define BIT_T_LOW  0
#define BIT_T_HIGH 1
#define BIT_T_CRIT 2

#define BIT_CONFIG_FAULT_QUEUE0 0
#define BIT_CONFIG_FAULT_QUEUE1 1
#define BIT_CONFIG_CT_POLARITY  2
#define BIT_CONFIG_INT_POLARITY 3
#define BIT_CONFIG_INT_CT_MODE  4
#define BIT_CONFIG_MODE0        5
#define BIT_CONFIG_MODE1        6
#define BIT_CONFIG_RESOLUTION   7

#define CHECK(x) ({ esp_err_t _err; if ((_err = (x)) != ESP_OK) return _err; })


static esp_err_t read_reg_16(adt7410_t *dev, uint8_t reg, uint16_t *val)
{
    I2C_DEV_TAKE_MUTEX(&dev->i2c_dev);
    I2C_DEV_CHECK(&dev->i2c_dev, i2c_dev_read_reg(&dev->i2c_dev, reg, val, 2));
    *val = (*val << 8) | (*val >> 8);
    I2C_DEV_GIVE_MUTEX(&dev->i2c_dev);

    return ESP_OK;
}

#if 0
static esp_err_t write_reg_16(adt7410_t *dev, uint8_t reg, uint16_t val)
{
    I2C_DEV_TAKE_MUTEX(&dev->i2c_dev);
    uint16_t buf = (val << 8) | (val >> 8);
    I2C_DEV_CHECK(&dev->i2c_dev, i2c_dev_write_reg(&dev->i2c_dev, reg, &buf, 2));
    I2C_DEV_GIVE_MUTEX(&dev->i2c_dev);

    return ESP_OK;
}
#endif

static esp_err_t update_reg_8(adt7410_t *dev, uint8_t reg, uint8_t *data, uint8_t mask, uint8_t or)
{
     uint8_t old;
     I2C_DEV_TAKE_MUTEX(&dev->i2c_dev);
     I2C_DEV_CHECK(&dev->i2c_dev, i2c_dev_read_reg(&dev->i2c_dev, reg, &old, 1));
     uint8_t new = (old & ~mask) | or;
     if (old != new)
          I2C_DEV_CHECK(&dev->i2c_dev, i2c_dev_write_reg(&dev->i2c_dev, reg, &new, 1));
     I2C_DEV_GIVE_MUTEX(&dev->i2c_dev);
     if (data) *data = new;
     return ESP_OK;
}

static esp_err_t read_reg_8(adt7410_t *dev, uint8_t reg, uint8_t *data)
{
        return update_reg_8(dev, reg, data, 0x00, 0x00);
}

static esp_err_t read_temp_16(adt7410_t *dev, uint8_t reg, float *t)
{
    int16_t v;
    CHECK(read_reg_16(dev, reg, (uint16_t *)&v));
    if (dev->res == ADT7410_RES_12)
         v &= ~0b111;
    *t = v;
    *t /= 128;
    return ESP_OK;
}

///////////////////////////////////////////////////////////////////////////////

esp_err_t adt7410_init_desc(adt7410_t *dev, uint8_t addr, i2c_port_t port, gpio_num_t sda_gpio, gpio_num_t scl_gpio)
{
    dev->i2c_dev.port = port;
    dev->i2c_dev.addr = addr;
    dev->i2c_dev.cfg.sda_io_num = sda_gpio;
    dev->i2c_dev.cfg.scl_io_num = scl_gpio;
#if HELPER_TARGET_IS_ESP32
    dev->i2c_dev.cfg.master.clk_speed = I2C_FREQ_HZ;
#endif

    return i2c_dev_create_mutex(&dev->i2c_dev);
}

esp_err_t adt7410_free_desc(adt7410_t *dev)
{
    return i2c_dev_delete_mutex(&dev->i2c_dev);
}

esp_err_t adt7410_init(adt7410_t *dev, adt7410_mode_t mode, adt7410_resolution_t res)
{
    uint8_t v;

    CHECK(read_reg_8(dev, REG_ID, &v));
    if ((v >> 3) != MANUFACTURER_ID) {
        ESP_LOGE(TAG, "Invalid device ID 0x%02x", v);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGD(TAG, "Device revision: 0x%02x", v & 0b111);

    CHECK(update_reg_8(dev, REG_CONF, NULL,
                       (1 << BIT_CONFIG_RESOLUTION)|(0b11 << BIT_CONFIG_MODE0),
                       (res << BIT_CONFIG_RESOLUTION)|(mode << BIT_CONFIG_MODE0)));
    dev->res = res;
    return ESP_OK;
}

esp_err_t adt7410_set_mode(adt7410_t *dev, adt7410_mode_t mode)
{
     return update_reg_8(dev, REG_CONF, NULL, 0b11 << BIT_CONFIG_MODE0, mode << BIT_CONFIG_MODE0);
}

esp_err_t adt7410_get_mode(adt7410_t *dev, adt7410_mode_t *mode)
{
    uint8_t v;

    CHECK(read_reg_8(dev, REG_ID, &v));
    *mode = (v >> BIT_CONFIG_MODE0) & 0b11;
    return ESP_OK;
}

esp_err_t adt7410_set_resolution(adt7410_t *dev, adt7410_resolution_t res)
{
     CHECK(update_reg_8(dev, REG_CONF, NULL, 1 << BIT_CONFIG_RESOLUTION, res << BIT_CONFIG_RESOLUTION));
     dev->res = res;
     return ESP_OK;
}

esp_err_t adt7410_get_resolution(adt7410_t *dev, adt7410_resolution_t *res)
{
    *res = dev->res;
    return ESP_OK;
}

esp_err_t adt7410_get_temperature(adt7410_t *dev, float *t)
{
    CHECK(read_temp_16(dev, REG_T_MSB, t));
    return ESP_OK;
}

