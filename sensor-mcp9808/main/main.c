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

#include <mcp9808.h>

#include "syslog.h"
#include "graphite.h"
#include "wifi.h"

static const char* TAG = "undefined";

static float system_vdd()
{
        uint16_t adc_data;
        esp_err_t err = adc_read(&adc_data);
        if (err == ESP_OK)  {
                float offset = 4.85;  // should be 5 actually (100kOhm + 220kOhm + 180kOhm) / 100 kOhm
                float vdd = (float)adc_data / 1000 * offset;
                ESP_LOGI(TAG, "Voltage: %.2fV", vdd);
                return vdd;
        }
        ESP_LOGE(TAG, "adc_read: failed %d", err);
        return 0;
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

#define ADDR MCP9808_I2C_ADDR_000
#define SDA_GPIO GPIO_NUM_4
#define SCL_GPIO GPIO_NUM_5
#define PWR_GPIO GPIO_NUM_15

        gpio_config(&(gpio_config_t){
                        .pin_bit_mask = BIT(PWR_GPIO),
                        .mode = GPIO_MODE_OUTPUT,
        });
        gpio_set_level(PWR_GPIO, 1); // power-on sensor module
        struct timeval mcp9808_poweron;
        gettimeofday(&mcp9808_poweron, NULL);

        // connect to WiFi before anything else. OTA must run _before_ any potentially buggy code
        wifi_connect();

        i2c_dev_t dev = {.port = 0};
        ESP_ERROR_CHECK(mcp9808_init_desc(&dev, ADDR, 0, SDA_GPIO, SCL_GPIO));
        ESP_ERROR_CHECK(mcp9808_init(&dev));

        struct timeval now;
        gettimeofday(&now, NULL);
        int poweron_duration_msec = (now.tv_sec - mcp9808_poweron.tv_sec) *  1000 +
                (now.tv_usec - mcp9808_poweron.tv_usec) / 1000;

        // mcp9808 needs 250ms to perform measurement
        int measurement_delay = (250 * 1.2) ;
        if (poweron_duration_msec <  measurement_delay)
                vTaskDelay((measurement_delay - poweron_duration_msec) / portTICK_RATE_MS);

        float temperature;
        esp_err_t res = mcp9808_get_temperature(&dev, &temperature, NULL, NULL, NULL);

        gpio_set_level(PWR_GPIO, 0); // power-off sensor module

        if (res == ESP_OK) {
                ESP_LOGI(TAG, "Temperature: %.2f°C", temperature);
                graphite("10.3.14.10", macstr("yaws.sensor_", ""),
                         (const char*[]){"temperature", "voltage", NULL},
                         (float[]){temperature, system_vdd()});
        } else {
                ESP_LOGE(TAG, "Could not get results: %d (%s)", res, esp_err_to_name(res));
        }

        vTaskDelay(200 / portTICK_RATE_MS); // TODO: better wait for send completion

        int sleep = 5 * 60;
        ESP_LOGD(TAG, "deep sleep for %d seconds", sleep);
        ESP_ERROR_CHECK(wifi_disconnect());
        esp_deep_sleep(sleep * 1000000);

}
