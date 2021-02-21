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

#include "daisy.h"

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

esp_err_t graphite(const char *ip, const char *prefix, const char **metric, float *value)
{
        struct sockaddr_in addr = {
                .sin_family = AF_INET,
                .sin_addr = (struct in_addr){
                        .s_addr = inet_addr(ip),
                },
                .sin_port = htons(2003)
        };

        if (addr.sin_addr.s_addr == INADDR_NONE) {
                ESP_LOGE(TAG, "Invalide graphite address: %s", ip);
                return ESP_FAIL;
        }

        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) {
                ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
                return ESP_FAIL;
        }

        int msglen = 0;
        char *msg = format(prefix, metric, value, &msglen);
        if (msg == NULL) {
                close(sock);
                return ESP_FAIL;
        }

        int n;
        for (int i = 0; i < 5; i++) {
                n = sendto(sock, msg, msglen, 0, (struct sockaddr *)&addr, sizeof addr);
                ESP_LOGD(TAG, "Sent %d bytes, message='%.*s'", n, msglen, msg);
                if (n == msglen)
                        break;
                ESP_LOGE(TAG, "Failed to send UDP datagram: errno %d, try %d", errno, i);
                vTaskDelay(200 / portTICK_RATE_MS);
        }
        if (n != msglen)
                ESP_LOGE(TAG, "Failed to send UDP datagram: errno %d", errno);

        free(msg);
        close(sock);

        if (n != msglen)
                return ESP_FAIL;
        return ESP_OK;
}

char *mac_prefix(const char *prefix)
{
        static char buf[64];
        if (buf[0] == 0) {
                snprintf(buf, sizeof buf - 1, "%s%02x%02x%02x%02x%02x%02x",
                         prefix,
                         mac_addr[0], mac_addr[1], mac_addr[2],
                         mac_addr[3], mac_addr[4], mac_addr[5]);
        }
        return buf;
}
