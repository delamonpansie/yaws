#include <string.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "hal/gpio_types.h"
#include "nvs_flash.h"
#include <bmp280.h>

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

static const char *TAG = "yaws";

#include "syslog.h"
#include "graphite.h"
#include "daisy.h"
#include "epaper.h"

#include "esp_http_client.h"

static void sensor()
{
        bmp280_params_t params;
        bmp280_init_default_params(&params);
        bmp280_t dev;
        memset(&dev, 0, sizeof(bmp280_t));

#define SCL_GPIO GPIO_NUM_21
#define SDA_GPIO GPIO_NUM_22

        gpio_set_direction(GPIO_NUM_23, GPIO_MODE_OUTPUT);
        gpio_set_level(GPIO_NUM_23, 1); // power-on sensor module

        ESP_ERROR_CHECK(bmp280_init_desc(&dev, BMP280_I2C_ADDRESS_1, 0, SDA_GPIO, SCL_GPIO));
        ESP_ERROR_CHECK(bmp280_init(&dev, &params));

        bool busy;
        do {
                ets_delay_us(250); // for whatever reason, bme280 doesn't start measuring right away
                ESP_ERROR_CHECK(bmp280_is_measuring(&dev, &busy));
        } while (busy);

        float pressure, temperature, humidity;
        int x = bmp280_read_float(&dev, &temperature, &pressure, &humidity);

        gpio_set_level(GPIO_NUM_23, 0); // power-off sensor module

        if (x != ESP_OK) {
                ESP_LOGE(TAG, "Temperature/pressure reading failed\n");
                return;
        }

        ESP_LOGI(TAG, "Temperature: %.2f, pressure: %.2f, humidity: %.2f", temperature, pressure, humidity);
        graphite("10.3.14.10", "yaws.bme280",
                 (const char*[]){"temperature", "pressure", "humidity", NULL},
                 (float[]){temperature, pressure, humidity});
}


RTC_DATA_ATTR char saved_etag[16] = {0};

uint8_t *get(const char *url, unsigned *len)
{
        uint8_t *buffer = NULL;
        int content_length;
        char *etag = NULL;

        esp_err_t event_handler(esp_http_client_event_t *ev)
        {
                if (ev->event_id == HTTP_EVENT_ON_HEADER)
                        if (strcmp(ev->header_key, "ETag") == 0)
                                strlcpy(saved_etag, ev->header_value, 16);
                return ESP_OK;
        }

        ESP_LOGI(TAG, "ETag: %s", saved_etag);

        esp_http_client_config_t config = {
                .url = url,
                .method = HTTP_METHOD_GET,
                .event_handler = event_handler,
                .user_data = &etag,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);

        if (*saved_etag)
                esp_http_client_set_header(client, "If-None-Match", saved_etag);

        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
                goto out;
        }
        content_length = esp_http_client_fetch_headers(client);
        if (content_length < 0) {
                ESP_LOGE(TAG, "HTTP client fetch headers failed");
                goto out;
        }
        *len = content_length;

        int code = esp_http_client_get_status_code(client);
        if (code != HttpStatus_Ok) {
                ESP_LOGI(TAG, "HTTP CODE %x", code);
                goto out;
        }

        if (content_length == 0) {
                goto out;
        }


        buffer = malloc(content_length);
        if (buffer == NULL) {
                ESP_LOGE(TAG, "Failed to allocate PNG buffer");
                goto out;
        }

        int data_read = esp_http_client_read_response(client, (char *)buffer, content_length);
        if (data_read != content_length) {
                ESP_LOGE(TAG, "Failed to read response");
                free(buffer);
                goto out;
        }

out:    esp_http_client_close(client);
        return buffer;
}


void display(const uint8_t *data, unsigned size)
{
        if (data == NULL)
                return;

        const unsigned ep_size = 800 * 480 / 8;
        if (size != ep_size) {
                ESP_LOGE(TAG, "Invalid bitmap size; got %d, want %d", size, ep_size);
                return;
        }

        epaper_conf_t epconf = {
                .reset_pin = GPIO_NUM_26,
                .dc_pin = GPIO_NUM_27,
                .cs_pin = GPIO_NUM_15,
                .busy_pin = GPIO_NUM_25,
                .mosi_pin = GPIO_NUM_14,
                .sck_pin = GPIO_NUM_13,

                .clk_freq_hz = SPI_MASTER_FREQ_20M,
                .spi_host = HSPI_HOST,
        };

        epaper_handle_t ep;
        ep = epaper_create(epconf);
        epaper_display(ep, data);
        epaper_delete(ep);
}

void app_main(void)
{
        /* const esp_app_desc_t *app_desc = esp_ota_get_app_description(); */
        /* TAG = app_desc->project_name; */
        log_early_init();

        esp_log_level_set("*", ESP_LOG_INFO);
        esp_log_level_set("esp_https_ota", ESP_LOG_INFO);
        esp_log_level_set(TAG, ESP_LOG_INFO);
        esp_log_level_set("yaws-wifi", ESP_LOG_INFO);
        esp_log_level_set("yaws-syslog", ESP_LOG_INFO);
        esp_log_level_set("yaws-graphite", ESP_LOG_DEBUG);

        ESP_ERROR_CHECK(nvs_flash_init());
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(i2cdev_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());

        log_init();

        ESP_ERROR_CHECK(wifi_connect());

        sensor();

        unsigned size;
        uint8_t *data = get("http://yaws.home.arpa/image.raw", &size);
        display(data, size);

        vTaskDelay(200 / portTICK_RATE_MS); // TODO: better wait for send completion

        int sleep = 1 * 60;
        ESP_LOGD(TAG, "deep sleep for %d seconds", sleep);
        ESP_ERROR_CHECK(wifi_disconnect());
        esp_deep_sleep(sleep * 1000000);

}
