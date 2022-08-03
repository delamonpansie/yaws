#include <string.h>
#include <math.h>

#include "driver/adc.h"
#include "driver/gpio.h"

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_adc_cal.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "syslog.h"
#include "graphite.h"
#include "wifi.h"
#include "epaper.h"

static const char *TAG = "undefined";


union fu {
	float f;
	u32_t u;
};

static float vdd;
static float vdd_read_raw()
{
        /* ADC1 channel 7 is GPIO35 */
        adc_power_acquire();

        adc1_config_width(ADC_WIDTH_BIT_12);
        adc1_config_channel_atten(ADC1_CHANNEL_7, ADC_ATTEN_DB_11);
        esp_adc_cal_characteristics_t characteristics;
        esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12,
                                 ESP_ADC_CAL_VAL_DEFAULT_VREF, &characteristics);
        uint32_t adc_raw = adc1_get_raw((adc1_channel_t)ADC1_CHANNEL_7);
        uint32_t mv = esp_adc_cal_raw_to_voltage(adc_raw, &characteristics);

	adc_power_release();
        return (float)mv / 1000;
}
static float vdd_offset()
{
        nvs_handle nvs;
        if (nvs_open(TAG, NVS_READONLY, &nvs) == ESP_OK) {
		union fu fu;
		esp_err_t err = nvs_get_u32(nvs, "vdd_offset", &fu.u);
		nvs_close(nvs);
		if (err == ESP_OK)
			return fu.f;
		ESP_LOGE(TAG, "nvs vdd_offset: %s", esp_err_to_name(err));
	}
	return 3.23;  // (150kOhm + 330kOhm) / 150 kOhm
}
static void vdd_read()
{
        vdd = vdd_read_raw() * vdd_offset();
}

static void vdd_offset_calibrate()
{
	float offset = vdd_offset();
	for (;;) {
		float new_offset = 4.2 / vdd_read_raw();
		ESP_LOGI(TAG, "VDD offset %f", new_offset);

		if (fabs(new_offset - offset) > 0.001) {
			nvs_handle nvs;
			esp_err_t err;
			if ((err = nvs_open(TAG, NVS_READWRITE, &nvs)) != ESP_OK)
				goto out;
			union fu fu = { .f = new_offset };
			err = nvs_set_u32(nvs, "vdd_offset", fu.u);
			if (err != ESP_OK)
				goto out;
			err = nvs_commit(nvs);
		out:
			if (err != ESP_OK)
				ESP_LOGE(TAG, "nvs vdd_offset: %s", esp_err_to_name(err));
			else
				offset = new_offset;
			nvs_close(nvs);
		}

		vTaskDelay(60000 / portTICK_PERIOD_MS);
	}
}

char RTC_DATA_ATTR saved_etag[16] = {0};

static esp_err_t event_handler(esp_http_client_event_t *ev)
{
        if (ev->event_id == HTTP_EVENT_ON_HEADER)
                if (strcmp(ev->header_key, "ETag") == 0)
                        strlcpy(saved_etag, ev->header_value, 16);
        return ESP_OK;
}

uint8_t *get(const char *url, unsigned *len)
{
        uint8_t *buffer = NULL;
        int content_length;
        char *etag = NULL;

        esp_http_client_config_t config = {
                .url = url,
                .method = HTTP_METHOD_GET,
                .event_handler = event_handler,
                .user_data = &etag,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);

        if (*saved_etag) {
                ESP_LOGD(TAG, "ETag: %s", saved_etag);
                esp_http_client_set_header(client, "If-None-Match", saved_etag);
        }

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
                ESP_LOGW(TAG, "HTTP CODE %d", code);
                goto out;
        }

        if (content_length == 0)
                goto out;

        buffer = malloc(content_length);
        if (buffer == NULL) {
                ESP_LOGE(TAG, "Failed to allocate picture buffer");
                goto out;
        }

        int data_read = esp_http_client_read_response(client, (char *)buffer, content_length);
        if (data_read != content_length) {
                ESP_LOGE(TAG, "Failed to read response");
                free(buffer);
                buffer = NULL;
                goto out;
        }

        ESP_LOGI(TAG, "GET %s fetched %d bytes", url, *len);
out:    esp_http_client_close(client);
        return buffer;
}


void display(const uint8_t *data, unsigned size)
{
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

volatile int RTC_DATA_ATTR ota_disabled;
volatile char RTC_DATA_ATTR last_err[32];

void app_main(void)
{
        const esp_app_desc_t *app_desc = esp_ota_get_app_description();
        TAG = app_desc->project_name;
        syslog_early_init();

        esp_log_level_set("*", ESP_LOG_WARN);
        esp_log_level_set("esp_https_ota", ESP_LOG_INFO);
        esp_log_level_set(TAG, ESP_LOG_INFO);
        esp_log_level_set("yaws-wifi", ESP_LOG_INFO);
        esp_log_level_set("yaws-syslog", ESP_LOG_INFO);
        esp_log_level_set("yaws-graphite", ESP_LOG_INFO);

        ESP_ERROR_CHECK(nvs_flash_init());
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());

        vdd_read();
        syslog_init();

        // connect to WiFi before anything else. OTA must run _before_ any potentially buggy code
        if (wifi_connect() != ESP_OK)
                goto sleep;

        // OTA source is checked only once after boot to save power.
        // If you want to force OTA: do a power cycle (reset is not enough).
        if (ota_disabled != 0x13131313) {
                char updated = 0;
                esp_err_t err = ota(&updated);
                if (err == ESP_OK || err == ESP_ERR_NOT_FOUND)
                        ota_disabled = 0x13131313;
                if (updated) {
                        vTaskDelay(100 / portTICK_PERIOD_MS);
                        esp_restart();
                }

		if (vdd_offset_calibration_requested())
			vdd_offset_calibrate();
        }

        unsigned size;
        uint8_t *data = get("http://yaws.home.arpa/image.raw", &size);
        if (data != NULL)
                display(data, size);

        const char *metric[] = {"voltage" , NULL};
        const float value[] = {vdd};
        graphite(macstr("yaws.sensor_", ""), metric, value);
        ESP_LOGI(TAG, "voltage: %0.2fV", vdd);
sleep:
        if (syslog_last_err[0])
                memcpy((char *)last_err, syslog_last_err, sizeof(last_err));

        wifi_disconnect();

        unsigned sleep_duration = 5 * 60 * 1000000;
        esp_deep_sleep(sleep_duration - esp_log_timestamp() * 1000);
}
