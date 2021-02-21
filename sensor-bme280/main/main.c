#include <stdio.h>
#include <string.h>

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

#include "syslog.h"
#include "graphite.h"
#include "daisy.h"

static const char* TAG = "undefined";

static char *sensor_prefix() {
        static char prefix[25];
        if (prefix[0] == 0) {
                int len = sprintf(prefix, "yaws.sensor_%02x%02x%02x%02x%02x%02x",
                                  mac_addr[0], mac_addr[1], mac_addr[2],
                                  mac_addr[3], mac_addr[4], mac_addr[5]);
                assert(len < sizeof prefix);
        }
        return prefix;
}

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

static void read_sensor()
{
        bmp280_params_t params;
        bmp280_init_default_params(&params);
        bmp280_t dev;
        memset(&dev, 0, sizeof(bmp280_t));

#define SDA_GPIO GPIO_NUM_4
#define SCL_GPIO GPIO_NUM_5

        /* gpio_set_direction(GPIO_NUM_23, GPIO_MODE_OUTPUT); */
        /* gpio_set_level(GPIO_NUM_23, 1); // power-on sensor module */

        ESP_ERROR_CHECK(bmp280_init_desc(&dev, BMP280_I2C_ADDRESS_1, 0, SDA_GPIO, SCL_GPIO));
        ESP_ERROR_CHECK(bmp280_init(&dev, &params));

        bool busy;
        do {
                ets_delay_us(250); // for whatever reason, bme280 doesn't start measuring right away
                ESP_ERROR_CHECK(bmp280_is_measuring(&dev, &busy));
        } while (busy);

        float pressure, temperature, humidity;
        int x = bmp280_read_float(&dev, &temperature, &pressure, &humidity);

        /* gpio_set_level(GPIO_NUM_23, 0); // power-off sensor module */

        if (x != ESP_OK) {
                ESP_LOGE(TAG, "Temperature/pressure reading failed\n");
                return;
        }

        ESP_LOGI(TAG, "Temperature: %.2f, pressure: %.2f, humidity: %.2f", temperature, pressure, humidity);
        graphite("10.3.14.10", sensor_prefix(),
                 (const char*[]){"temperature", "pressure", "humidity", "voltage", NULL},
                 (float[]){temperature, pressure, humidity, system_vdd()});
}

void app_main()
{
        const esp_app_desc_t *app_desc = esp_ota_get_app_description();
        TAG = app_desc->project_name;
        log_early_init();

        esp_log_level_set("*", ESP_LOG_WARN);
        esp_log_level_set("esp_https_ota", ESP_LOG_INFO);
        // esp_log_level_set("HTTP_CLIENT", ESP_LOG_DEBUG);
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
        wifi_connect();

        read_sensor();

        vTaskDelay(200 / portTICK_RATE_MS); // TODO: better wait for send completion

        int sleep = 5 * 60;
        ESP_LOGD(TAG, "deep sleep for %d seconds", sleep);
        ESP_ERROR_CHECK(wifi_disconnect());
        esp_deep_sleep(sleep * 1000000);

}
