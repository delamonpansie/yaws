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
#include "daisy.h"
#include "main.h"

static const char* TAG = "undefined";

static float system_vdd()
{
        uint16_t adc_data;
        esp_err_t err = adc_read(&adc_data);
        if (err == ESP_OK)  {
                float vdd = (float)adc_data / 1000;
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

        esp_log_level_set("*", ESP_LOG_INFO);
        esp_log_level_set("esp_https_ota", ESP_LOG_INFO);
        esp_log_level_set(TAG, ESP_LOG_INFO);
        esp_log_level_set("yaws-wifi", ESP_LOG_INFO);
        esp_log_level_set("yaws-syslog", ESP_LOG_INFO);

        ESP_ERROR_CHECK(nvs_flash_init());
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        ESP_ERROR_CHECK(adc_init(&(adc_config_t){.mode = ADC_READ_VDD_MODE, .clk_div = 8}));
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

        struct timeval tv_now;
        gettimeofday(&tv_now, NULL);

        wifi_connect();

#define ADDR MCP9808_I2C_ADDR_000
#define SDA_GPIO GPIO_NUM_4
#define SCL_GPIO GPIO_NUM_5
#define PWR_GPIO GPIO_NUM_15

        gpio_config(&(gpio_config_t){
                        .pin_bit_mask = BIT(PWR_GPIO),
                        .mode = GPIO_MODE_OUTPUT,
        });
        gpio_set_level(PWR_GPIO, 1); // power-on sensor module

        i2c_dev_t dev = {.port = 0};
        ESP_ERROR_CHECK(mcp9808_init_desc(&dev, ADDR, 0, SDA_GPIO, SCL_GPIO));
        ESP_ERROR_CHECK(mcp9808_init(&dev));

        vTaskDelay(300 / portTICK_RATE_MS); // wait for measurement
        float temperature;
        esp_err_t res = mcp9808_get_temperature(&dev, &temperature, NULL, NULL, NULL);

        gpio_set_level(PWR_GPIO, 0); // power-off sensor module

        if (res == ESP_OK) {
                ESP_LOGI(TAG, "Temperature: %.2f°C", temperature);
                graphite("10.3.14.10", mac_prefix("yaws.sensor_"),
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
