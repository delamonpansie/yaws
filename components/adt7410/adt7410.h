/**
 * ESP-IDF driver for ADT7410 Digital Temperature Sensor
 *
 */
#ifndef __ADT7410_H__
#define __ADT7410_H__

#include <stdbool.h>
#include <i2cdev.h>

/* #ifdef __cplusplus */
/* extern "C" { */
/* #endif */

#define ADT7410_I2C_ADDR_000 0x48 //!< I2C address, pins: A1=0, A0=0
#define ADT7410_I2C_ADDR_001 0x49 //!< I2C address, pins: A1=1, A0=1
#define ADT7410_I2C_ADDR_010 0x4A //!< I2C address, pins: A1=1, A0=0
#define ADT7410_I2C_ADDR_011 0x4B //!< I2C address, pins: A1=1, A0=1

/**
 * Device mode
 */
typedef enum {
    ADT7410_CONTINUOUS, //!< Continuous conversion, default
    ADT7410_ONE_SHOT,   //!< One shot mode, conversion time is typically 240 ms
    ADT7410_1SPC,       //!< One measurement per second,  conversion time is typically 60 ms
    ADT7410_SHUTDOWN    //!< Shutdown mode
} adt7410_mode_t;


/**
 * Temperature resolution
 */
typedef enum {
    ADT7410_RES_12 = 0, //!< Resolution = +0.0625°C
    ADT7410_RES_16,     //!< Resolution = +0.0078°C
} adt7410_resolution_t;


/**
 * Device descriptor
 */
typedef struct {
    i2c_dev_t i2c_dev;         //!< I2C device descriptor
    adt7410_resolution_t res;  //!< Currently configured resolution
} adt7410_t;

/**
 * @brief Initialize device descriptor
 *
 * @param dev Device descriptor
 * @param addr Device I2C address
 * @param port I2C port
 * @param sda_gpio SDA GPIO
 * @param scl_gpio SCL GPIO
 * @return `ESP_OK` on success
 */
esp_err_t adt7410_init_desc(adt7410_t *dev, uint8_t addr, i2c_port_t port, gpio_num_t sda_gpio, gpio_num_t scl_gpio);

/**
 * @brief Free device descriptor
 *
 * @param dev Device descriptor
 * @return `ESP_OK` on success
 */
esp_err_t adt7410_free_desc(adt7410_t *dev);

/**
 * @brief Init device
 *
 * Set device configuration to default, clear lock bits
 *
 * @param dev Device descriptor
 * @param mode Temperature sampling mode
 * @param res Resolution mode
 * @return `ESP_OK` on success
 */
esp_err_t adt7410_init(adt7410_t *dev, adt7410_mode_t mode, adt7410_resolution_t res);

/**
 * @brief Set device mode
 *
 * @param dev Device descriptor
 * @param mode Power mode
 * @return `ESP_OK` on success
 */
esp_err_t adt7410_set_mode(adt7410_t *dev, adt7410_mode_t mode);

/**
 * @brief Get device mode
 *
 * @param dev Device descriptor
 * @param[out] mode Current power mode
 * @return `ESP_OK` on success
 */
esp_err_t adt7410_get_mode(adt7410_t *dev, adt7410_mode_t *mode);

/**
 * @brief Set temperature resolution
 *
 * @param dev Device descriptor
 * @param res Resolution
 * @return `ESP_OK` on success
 */
esp_err_t adt7410_set_resolution(adt7410_t *dev, adt7410_resolution_t res);

/**
 * @brief Get temperature resolution
 *
 * @param dev Device descriptor
 * @param[out] res Resolution
 * @return `ESP_OK` on success
 */
esp_err_t adt7410_get_resolution(adt7410_t *dev, adt7410_resolution_t *res);

/**
 * @brief Read temperature
 *
 * @param dev Device descriptor
 * @param[out] t Ambient temperature
 * @return `ESP_OK` on success
 */
esp_err_t adt7410_get_temperature(adt7410_t *dev, float *t);

#ifdef __cplusplus
}
#endif

/**@}*/

#endif /* __ADT7410_H__ */
