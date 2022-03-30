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

static char *format(const char *prefix, const char **metric, const float *value, int *msglen)
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


esp_err_t graphite(const char *prefix, const char **metric, const float *value)
{
        struct sockaddr_in addr = {
                .sin_family = AF_INET,
                .sin_addr = (struct in_addr){
                        .s_addr = inet_addr(CONFIG_GRAPHITE_ADDR),
                },
                .sin_port = htons(2003)
        };

        if (addr.sin_addr.s_addr == INADDR_NONE) {
                ESP_LOGE(TAG, "invalid CONFIG_GRAPHITE_ADDR: %s", CONFIG_GRAPHITE_ADDR);
                return ESP_FAIL;
        }

        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (sock < 0) {
                ESP_LOGE(TAG, "socket: %s", strerror(errno));
                return ESP_FAIL;
        }

        if (connect(sock, (struct sockaddr *)&addr, sizeof addr) < 0) {
                ESP_LOGE(TAG, "connect: %s", strerror(errno));
                return ESP_FAIL;
        }

        int msglen = 0;
        char *msg = format(prefix, metric, value, &msglen);
        if (msg == NULL) {
                ESP_LOGI(TAG, "graphite: formatting failed");
                return ESP_FAIL;
        }

        char *ptr = msg;
        while (msglen > 0) {
                int n = send(sock, ptr, msglen, 0);
                if (n < 0) {
                        ESP_LOGE(TAG, "send: %s", strerror(errno));
                        break;
                }
                ptr += n;
                msglen -= n;
        }

        free(msg);
        close(sock);

        return ESP_OK;
}
