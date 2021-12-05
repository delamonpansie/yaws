#include <stdio.h>
#include <string.h>

#include "driver/adc.h"
#include "driver/i2c.h"
#include "driver/gpio.h"

#include "esp_event.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <bmp280.h>
#include <mcp9808.h>

#include "syslog.h"
#include "graphite.h"
#include "wifi.h"

static const char* TAG = "undefined";

#define I2C_PORT 0
#define SDA_GPIO GPIO_NUM_4  // pin D2
#define SCL_GPIO GPIO_NUM_5  // pin D1
#define PWR_GPIO GPIO_NUM_13 // pin D7

#if CONFIG_NEWLIB_NANO_FORMAT
# error newlib nano format library does not support formatting floats
#endif

static float system_vdd()
{
        uint16_t adc_data;
        esp_err_t err = adc_read(&adc_data);
        if (err == ESP_OK)  {
                float offset = 4.85;  // should be 5 actually (100kOhm + 220kOhm + 180kOhm) / 100 kOhm
                float vdd = (float)adc_data / 1000 * offset;
                if (vdd > 0.5)
                        ESP_LOGI(TAG, "Voltage: %0.2fV", vdd);
                return vdd;
        }
        ESP_LOGE(TAG, "adc_read: failed %d", err);
        return 0;
}

static void read_bme280(struct timeval *poweron __attribute__(()))
{
        bmp280_params_t params;
        bmp280_init_default_params(&params);
        bmp280_t dev;
        memset(&dev, 0, sizeof(bmp280_t));

        ESP_ERROR_CHECK(bmp280_init_desc(&dev, BMP280_I2C_ADDRESS_1, I2C_PORT, SDA_GPIO, SCL_GPIO));
        ESP_ERROR_CHECK(bmp280_init(&dev, &params));

        bool busy;
        do {
                ets_delay_us(250); // for whatever reason, bme280 doesn't start measuring right away
                ESP_ERROR_CHECK(bmp280_is_measuring(&dev, &busy));
        } while (busy);

        float pressure, temperature, humidity;
        int x = bmp280_read_float(&dev, &temperature, &pressure, &humidity);

        gpio_set_level(PWR_GPIO, 0); // power-off sensor module

        if (x != ESP_OK) {
                ESP_LOGE(TAG, "Temperature/pressure reading failed\n");
                return;
        }

        ESP_LOGI(TAG, "Temperature: %.2f, pressure: %.2f, humidity: %.2f", temperature, pressure, humidity);
        graphite(macstr("yaws.sensor_", ""),
                 (const char*[]){"temperature", "pressure", "humidity", "voltage", NULL},
                 (const float[]){temperature, pressure, humidity, system_vdd()});
}

static void read_mcp9808(struct timeval *poweron)
{
        i2c_dev_t dev = {.port = 0};
        ESP_ERROR_CHECK(mcp9808_init_desc(&dev, MCP9808_I2C_ADDR_000, I2C_PORT, SDA_GPIO, SCL_GPIO));
        ESP_ERROR_CHECK(mcp9808_init(&dev));

        float temperature = 0;
        esp_err_t res = mcp9808_get_temperature(&dev, &temperature, NULL, NULL, NULL);

        struct timeval now;
        gettimeofday(&now, NULL);
        int poweron_duration_msec = (now.tv_sec - poweron->tv_sec) *  1000 + (now.tv_usec - poweron->tv_usec) / 1000;

        // mcp9808 needs 250ms to perform measurement
        const int measurement_delay_msec = 250 * 1.2; // +20% tolerance
        if (poweron_duration_msec <  measurement_delay_msec)
                vTaskDelay((measurement_delay_msec - poweron_duration_msec) / portTICK_RATE_MS);

        gpio_set_level(PWR_GPIO, 0); // power-off sensor module

        if (res == ESP_OK) {
                ESP_LOGI(TAG, "Temperature: %.2f°C", temperature);

                const char *metric[] = {"temperature", "voltage", NULL};
                const float value[] = {temperature, system_vdd() };
                if (value[1] < 0.5) // ADC is not connected, do not send vdd measurment
                        metric[1] = NULL;

                graphite(macstr("yaws.sensor_", ""), metric, value);
        } else {
                ESP_LOGE(TAG, "Could not get results: %d (%s)", res, esp_err_to_name(res));
        }
}

static uint8_t i2c_addr_detect()
{
        uint8_t addr_list[] = { BMP280_I2C_ADDRESS_1, MCP9808_I2C_ADDR_000, 0x80 };

        i2c_config_t i2c = {
                .mode = I2C_MODE_MASTER,
                .sda_io_num = SDA_GPIO,
                .scl_io_num = SCL_GPIO,
                // sensors have pullups installed
        };

        esp_err_t res;

#if HELPER_TARGET_IS_ESP32
        i2c.master.clk_speed = I2C_FREQ_HZ;
        ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &i2c));
        if ((res = i2c_driver_install(I2C_PORT, i2c.mode, 0, 0, 0)) != ESP_OK)
            return 0x80;
#endif
#if HELPER_TARGET_IS_ESP8266
#if HELPER_TARGET_VERSION > HELPER_TARGET_VERSION_ESP8266_V3_2
        // Clock Stretch time, depending on CPU frequency
        i2c.clk_stretch_tick = I2CDEV_MAX_STRETCH_TIME;
#endif
        if ((res = i2c_driver_install(I2C_PORT, i2c.mode)) != ESP_OK)
            return 0x80;
        ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &i2c));
#endif
        int result = 0x80; // return 0x80 in case of errror, no valid i2c addr can have highest bit set
        for (uint8_t *addr = addr_list; *addr != 0x80; addr++) {
                i2c_cmd_handle_t cmd = i2c_cmd_link_create();
                ESP_ERROR_CHECK(i2c_master_start(cmd));
                ESP_ERROR_CHECK(i2c_master_write_byte(cmd, *addr << 1, true));
                ESP_ERROR_CHECK(i2c_master_stop(cmd));
                esp_err_t check = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(CONFIG_I2CDEV_TIMEOUT));
                i2c_cmd_link_delete(cmd);

                if (check == ESP_OK) {
                        ESP_LOGI(TAG, "detected I2C sensor with 0x%02x addr", *addr);
                        result = *addr;
                        break;
                }
        }

        ESP_ERROR_CHECK(i2c_driver_delete(I2C_PORT));
        return result;
}

static esp_err_t i2c_addr_store(uint8_t addr)
{
        nvs_handle_t nvs;
        esp_err_t err;
        if ((err = nvs_open(TAG, NVS_READWRITE, &nvs)) != ESP_OK)
                goto out;
        if (addr == 0x80)
                err = nvs_erase_key(nvs, "i2c_addr");
        else
                err = nvs_set_u8(nvs, "i2c_addr", addr);
        if (err != ESP_OK)
                goto out;
        err = nvs_commit(nvs);
out:
        nvs_close(nvs);
        return err;
}

static esp_err_t i2c_addr_load(uint8_t *addr)
{
        nvs_handle_t nvs;
        esp_err_t err;
        if ((err = nvs_open(TAG, NVS_READONLY, &nvs)) != ESP_OK)
                return err;
        err = nvs_get_u8(nvs, "i2c_addr", addr);
        nvs_close(nvs);
        return err;
}

static uint8_t i2c_addr()
{
        uint8_t addr;
        esp_err_t err;

        err = i2c_addr_load(&addr);
        if (err == ESP_OK) {
                ESP_LOGI(TAG, "loaded I2C sensor addr 0x%02x from NVS", addr);
                return addr;
        }

        ESP_LOGE(TAG, "failed to load i2c_addr from NVS: %s", esp_err_to_name(err));

        addr = i2c_addr_detect();
        if (addr == 0x80) {
                ESP_LOGI(TAG, "failed to detect I2C sensor");
                return addr;
        }

        if (err == ESP_ERR_NVS_NOT_FOUND) {
                err = i2c_addr_store(addr);
                if (err != ESP_OK)
                        ESP_LOGE(TAG, "failed to store i2c_addr to NVS: %s", esp_err_to_name(err));
        }

        return addr;
}

void app_main()
{
        const esp_app_desc_t *app_desc = esp_ota_get_app_description();
        TAG = app_desc->project_name;
        log_early_init();

        esp_log_level_set("*", ESP_LOG_ERROR);
        esp_log_level_set("esp_https_ota", ESP_LOG_INFO);
        esp_log_level_set(TAG, ESP_LOG_INFO);
        esp_log_level_set("yaws-wifi", ESP_LOG_INFO);
        esp_log_level_set("yaws-syslog", ESP_LOG_INFO);

        ESP_ERROR_CHECK(nvs_flash_init());
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        ESP_ERROR_CHECK(adc_init(&(adc_config_t){.mode = ADC_READ_TOUT_MODE, .clk_div = 8}));
        ESP_ERROR_CHECK(i2cdev_init());

        ESP_LOGI(TAG, "version: %s", app_desc->version);

#if CONFIG_PM_ENABLE
        // Configure dynamic frequency scaling:
        // maximum and minimum frequencies are set in sdkconfig,
        // automatic light sleep is enabled if tickless idle support is enabled.
        esp_pm_config_esp8266_t pm_config = {
                .light_sleep_enable = true
        };
        ESP_ERROR_CHECK( esp_pm_configure(&pm_config) );
#endif // CONFIG_PM_ENABLE

        log_init();

        gpio_config_t cfg = {
                .pin_bit_mask = BIT(PWR_GPIO),
                .mode = GPIO_MODE_OUTPUT,
        };
        ESP_ERROR_CHECK(gpio_config(&cfg));
        ESP_ERROR_CHECK(gpio_set_level(PWR_GPIO, 1)); // power-on sensor module
        struct timeval poweron;
        gettimeofday(&poweron, NULL);


        // connect to WiFi before anything else. OTA must run _before_ any potentially buggy code
        if (wifi_connect() != ESP_OK)
                goto sleep;

        if (ota() == ESP_OK) {
                // force I2C redetection on OTA
                uint8_t addr = i2c_addr_detect();
                if (addr != 0x80)
                        i2c_addr_store(addr);

                vTaskDelay(100 / portTICK_RATE_MS);
                esp_restart();
        }

        graphite_init();

        uint8_t addr = i2c_addr();
        switch (addr) {
        case BMP280_I2C_ADDRESS_1: read_bme280(&poweron); break;
        case MCP9808_I2C_ADDR_000: read_mcp9808(&poweron); break;
        case 0x80:
                ESP_LOGE(TAG, "unable to find I2C sensor");
                break;
        default:
                ESP_LOGE(TAG, "unknown sensor addr 0x%02x", addr);
        }

        gpio_set_level(PWR_GPIO, 0); // power-off sensor module

        vTaskDelay(200 / portTICK_RATE_MS); // TODO: better wait for send completion
        ESP_ERROR_CHECK(wifi_disconnect());

sleep:
        ESP_LOGI(TAG, "deep sleep");
        esp_deep_sleep(5 * 60 * 1000000);
}