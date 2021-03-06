#include <string.h>

#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "lwip/dhcp.h"
#include "esp_netif.h"
#if defined(CONFIG_IDF_TARGET_ESP8266)
typedef struct netif esp_netif_t;
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "daisy.h"

static const char *ota_base = "yaws.home.arpa/ota/";

static const char *TAG = "yaws-wifi";

static EventGroupHandle_t status;
ip4_addr_t ip_addr;
uint8_t mac_addr[6];
char bootp[DHCP_BOOT_FILE_LEN];

static esp_netif_t *netif = NULL;
static wifi_config_t wifi_config = {
        .sta = {
#include "secrets.txt"
        }
};

static void on_wifi_disconnect(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
        system_event_sta_disconnected_t *event = event_data;
        ESP_LOGI(TAG, "Wi-Fi disconnected, reason: %d", event->reason);
        xEventGroupSetBits(status, BIT(2));
}

static void on_got_ip(void *arg, esp_event_base_t event_base,
                      int32_t event_id, void *event_data)
{
        ip_event_got_ip_t *event = event_data;
#if defined(CONFIG_IDF_TARGET_ESP32)
        if (event->esp_netif != netif)
                return;
#endif
        memcpy(&ip_addr, &event->ip_info.ip, sizeof(ip_addr));
        ESP_ERROR_CHECK(esp_wifi_get_mac(ESP_IF_WIFI_STA, mac_addr));

        struct dhcp *dhcp = netif_dhcp_data(netif);
        memset(bootp, 0, sizeof bootp);
        memcpy(bootp, dhcp->boot_file_name, sizeof bootp - 1);

        xEventGroupSetBits(status, BIT(1));
}

#if defined(CONFIG_IDF_TARGET_ESP32)
static void on_shutdown()
{
        wifi_disconnect();
}
#endif

static char *ota_same_version = "same";
static char *ota_url(const char *base)
{
        const esp_app_desc_t *app_desc = esp_ota_get_app_description();
        static char url[DHCP_BOOT_FILE_LEN];
        char *result = NULL;

        snprintf(url, sizeof url, "http://%s%s.version", base, app_desc->project_name);
        ESP_LOGI(TAG, "OTA checking %s", url);
        esp_http_client_config_t client_config = {
                .url = url,
                .method = HTTP_METHOD_GET,
        };
        esp_http_client_handle_t client = esp_http_client_init(&client_config);

        esp_err_t err = esp_http_client_perform(client);
        if (err != ESP_OK) {
                ESP_LOGE(TAG, "OTA version fetch failed: http_client_perform returned %d", err);
                goto out;
        }

        int status_code = esp_http_client_get_status_code(client);
        if (status_code != 200) {
                ESP_LOGE(TAG, "OTA version %s fetch failed: status code %d", url, status_code);
                goto out;
        }


        char version[33] = {0,};
        esp_http_client_read(client, version, sizeof version - 1);
        for (char *p = version; *p; p++) {
                if (isspace(*p)) {
                        *p = 0;
                        break;
                }
        }

        ESP_LOGI(TAG, "OTA remote version: %s, local version: %s", version, app_desc->version);
        if (strcmp(version, app_desc->version) == 0) {
                result = ota_same_version;
                goto out;
        }

        snprintf(url, sizeof url, "http://%s%s.bin", base, app_desc->project_name);
        result = url;
out:
        esp_http_client_cleanup(client);
        return result;
}

static void ota()
{
        static RTC_DATA_ATTR char ota_disabled;
        if (ota_disabled == 0x13)
                return;
        ota_disabled = 0x13;

        char *url = NULL;
        if (memcmp(bootp, "http://", 7) == 0)
                url = bootp;

        if (url == NULL)
                url = ota_url(macstr(ota_base, "/"));

        if (url == NULL)
                url = ota_url(ota_base);

        if (url == NULL || url == ota_same_version)
                return;

        ESP_LOGI(TAG, "OTA %s", url);
        esp_err_t ret = esp_https_ota(&(esp_http_client_config_t){.url = url, .method = HTTP_METHOD_GET});
        switch (ret) {
        case ESP_OK:
                ESP_LOGI(TAG, "OTA completed successfully, rebooting");
                vTaskDelay(100 / portTICK_RATE_MS);
                esp_restart();
        default:
                ESP_LOGE(TAG, "Firmware upgrade failed");
                return;
        case ESP_ERR_NOT_FOUND:
                ESP_LOGI(TAG, "Firmware not found");
                return;
        }
}

esp_err_t wifi_connect(void)
{
        if (status != NULL)
                return ESP_ERR_INVALID_STATE;
#if defined(CONFIG_IDF_TARGET_ESP32)
        ESP_ERROR_CHECK(esp_register_shutdown_handler(&on_shutdown));
#endif
        status = xEventGroupCreate();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MAX_MODEM));

        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &on_wifi_disconnect, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_got_ip, NULL));

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));

        ESP_ERROR_CHECK(esp_wifi_start());


#if defined(CONFIG_IDF_TARGET_ESP8266)
        ESP_ERROR_CHECK(tcpip_adapter_get_netif(TCPIP_ADAPTER_IF_STA, (void **)&netif)); // must be called after esp_wifi_start()

#elif defined(CONFIG_IDF_TARGET_ESP32)
        netif = esp_netif_create_default_wifi_sta();
#endif
        ESP_ERROR_CHECK(esp_wifi_connect());

        EventBits_t bits = xEventGroupWaitBits(status, BIT(1)|BIT(2), true, false, 10000 / portTICK_RATE_MS);
        if ((bits & BIT(1)) == 0) {
                ESP_LOGE(TAG, "Wi-Fi connction failed");
                esp_deep_sleep(10 * 1000000);
        }

        ota();

        return ESP_OK;
}

int wifi_connected() {
        return xEventGroupGetBits(status) & BIT(1);
}

esp_err_t wifi_disconnect(void)
{
        if (status == NULL)
                return ESP_ERR_INVALID_STATE;

        vEventGroupDelete(status);
        status = NULL;

#if defined(CONFIG_IDF_TARGET_ESP32)
        ESP_ERROR_CHECK(esp_unregister_shutdown_handler(&on_shutdown));
#endif
        ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &on_wifi_disconnect));
        ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_got_ip));
        esp_err_t err = esp_wifi_stop();
        if (err == ESP_ERR_WIFI_NOT_INIT)
                return ESP_OK;

        ESP_ERROR_CHECK(err);
        ESP_ERROR_CHECK(esp_wifi_deinit());

#if defined(CONFIG_IDF_TARGET_ESP32)
        if (netif != NULL) {
                ESP_ERROR_CHECK(esp_wifi_clear_default_wifi_driver_and_handlers(netif));
                esp_netif_destroy(netif);
                netif = NULL;
        }
#endif
        return ESP_OK;
}

char *macstr(const char *prefix, const char *suffix)
{
        static char buf[64];
        snprintf(buf, sizeof buf - 1, "%s"MACSTR"%s", prefix, MAC2STR(mac_addr), suffix);
        return buf;
}
