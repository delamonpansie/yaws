#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "driver/adc.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <bmp280.h>
#include <mcp9808.h>

#include "syslog.h"
#include "graphite.h"
#include "wifi.h"

static const char* TAG = "undefined";

#define SDA_GPIO GPIO_NUM_4  // pin D2
#define SCL_GPIO GPIO_NUM_5  // pin D1
#define PWR_GPIO GPIO_NUM_13 // pin D7

static float system_vdd()
{
        uint16_t adc_data;
        esp_err_t err = adc_read(&adc_data);
        if (err == ESP_OK)  {
                float offset = 4.85;  // should be 5 actually (100kOhm + 220kOhm + 180kOhm) / 100 kOhm
                float vdd = (float)adc_data / 1000 * offset;
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

        ESP_ERROR_CHECK(bmp280_init_desc(&dev, BMP280_I2C_ADDRESS_1, 0, SDA_GPIO, SCL_GPIO));
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
                 (float[]){temperature, pressure, humidity, system_vdd()});
}

static void read_mcp9808(struct timeval *poweron)
{
#define ADDR MCP9808_I2C_ADDR_000

        i2c_dev_t dev = {.port = 0};
        ESP_ERROR_CHECK(mcp9808_init_desc(&dev, ADDR, 0, SDA_GPIO, SCL_GPIO));
        ESP_ERROR_CHECK(mcp9808_init(&dev));

        struct timeval now;
        gettimeofday(&now, NULL);
        int poweron_duration_msec = (now.tv_sec - poweron->tv_sec) *  1000 +
                (now.tv_usec - poweron->tv_usec) / 1000;

        // mcp9808 needs 250ms to perform measurement
        int measurement_delay = (250 * 1.2) ;
        if (poweron_duration_msec <  measurement_delay)
                vTaskDelay((measurement_delay - poweron_duration_msec) / portTICK_RATE_MS);

        float temperature = 0;
        esp_err_t res = mcp9808_get_temperature(&dev, &temperature, NULL, NULL, NULL);

        // gpio_set_level(PWR_GPIO, 0); // power-off sensor module

        if (res == ESP_OK) {
                ESP_LOGI(TAG, "Temperature: %.2f°C", temperature);
                graphite(macstr("yaws.sensor_", ""),
                         (const char*[]){"temperature", "voltage", NULL},
                         (float[]){temperature, system_vdd()});
        } else {
                ESP_LOGE(TAG, "Could not get results: %d (%s)", res, esp_err_to_name(res));
        }
}


static int sensor_type()
{
        static RTC_DATA_ATTR char cached_type;
        if ((cached_type & 0x80) == 0) {
                gpio_config_t cfg = {
                        .pin_bit_mask = BIT(GPIO_NUM_12)|BIT(GPIO_NUM_14), // pin D6, pin D5
                        .mode = GPIO_MODE_INPUT,
                        .pull_up_en = GPIO_PULLUP_ENABLE,
                };
                ESP_ERROR_CHECK(gpio_config(&cfg));
                cached_type = 0x80|gpio_get_level(GPIO_NUM_12)<<1|gpio_get_level(GPIO_NUM_14);
                cfg.pull_up_en = GPIO_PULLUP_DISABLE;
                ESP_ERROR_CHECK(gpio_config(&cfg));
        }
        return cached_type & 0x7f;
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

        gpio_set_direction(PWR_GPIO, GPIO_MODE_OUTPUT);
        gpio_set_level(PWR_GPIO, 1); // power-on sensor module

        struct timeval poweron;
        gettimeofday(&poweron, NULL);

        // connect to WiFi before anything else. OTA must run _before_ any potentially buggy code
        wifi_connect();
        graphite_init();

        switch (sensor_type()) {
        case 1: read_bme280(&poweron); break;
        case 2: read_mcp9808(&poweron); break;
        default:
                ESP_LOGI(TAG, "unknown sensor type %d", sensor_type());
        }

        gpio_set_level(PWR_GPIO, 0); // power-off sensor module

        vTaskDelay(200 / portTICK_RATE_MS); // TODO: better wait for send completion

        int sleep = 5 * 60;
        ESP_LOGD(TAG, "deep sleep for %d seconds", sleep);
        ESP_ERROR_CHECK(wifi_disconnect());
        esp_deep_sleep(sleep * 1000000);

}
