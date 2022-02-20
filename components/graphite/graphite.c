#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include "freertos/message_buffer.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

#include "wifi.h"

static const char* TAG = "yaws-graphite";

static char *format(const char *prefix, const char **metric, float *value, int *msglen)
{
        int prefix_len = strlen(prefix);
        int len = 0;
        for (const char **m = metric; *m; m++) {
                len += prefix_len;
                len += 1;          // '.'
                len += strlen(*m);
                len += 1;          // ' '
                len += 24;         // enough to represent double
                len += 4;          // ' -1\n'
        }

        char *buf = malloc(len);
        if (buf == NULL) {
                ESP_LOGE(TAG, "Unable to allocate memory for message buffer");
                return NULL;
        }

        char *w = buf;
        for (const char **m = metric; *m; m++) {
                w += sprintf(w, "%s.%s %f -1\n", prefix, *m, *value++);
                assert(w - buf < len);
        }
        *msglen = w - buf;

        if (*msglen > 0xffff - 28) {
                ESP_LOGE(TAG, "Too many metrics: packet size %d exceeds %d", *msglen, 0xffff - 28);
                free(buf);
                return NULL ;
        }

        return buf;
}

static int sock = -1;
static struct sockaddr_in addr;

static esp_err_t graphite_init()
{
        addr = (struct sockaddr_in) {
                .sin_family = AF_INET,
                .sin_addr = (struct in_addr){
                        .s_addr = inet_addr(CONFIG_GRAPHITE_ADDR),
                },
                .sin_port = htons(2003)
        };

        if (addr.sin_addr.s_addr == INADDR_NONE) {
                ESP_LOGE(TAG, "Invalid graphite address: %s", CONFIG_GRAPHITE_ADDR);
                return ESP_FAIL;
        }

        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) {
                ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
                return ESP_FAIL;
        }

        return ESP_OK;
}

esp_err_t graphite(const char *prefix, const char **metric, float *value)
{
        if (sock < 0) {
                esp_err_t err = graphite_init();
                if (err != ESP_OK)
                        return err;
        }

        int msglen = 0;
        char *msg = format(prefix, metric, value, &msglen);
        if (msg == NULL) {
                ESP_LOGI(TAG, "Formatting failed");
                return ESP_FAIL;
        }

        int n;
        for (;;) {
                n = sendto(sock, msg, msglen, 0, (struct sockaddr *)&addr, sizeof addr);
                ESP_LOGD(TAG, "Sent %d bytes, message='%.*s'", n, msglen, msg);
                if (n == msglen)
                        break;
                vTaskDelay(250 / portTICK_PERIOD_MS);
        }
        free(msg);

        return ESP_OK;
}
