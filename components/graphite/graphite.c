#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef CONFIG_IDF_TARGET_ESP8266
#include "esp_aio.h"
#endif
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
                ESP_LOGE(TAG, "invalid CONFIG_GRAPHITE_ADDR: %s", CONFIG_GRAPHITE_ADDR);
                return ESP_FAIL;
        }

        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) {
                ESP_LOGE(TAG, "socket: errno %s", strerror(errno));
                return ESP_FAIL;
        }

        return ESP_OK;
}

#ifdef CONFIG_IDF_TARGET_ESP8266
static int packet_tx_status;
static void track_packet_status(const esp_aio_t *aio)
{
	wifi_tx_status_t *status = (wifi_tx_status_t *) &(aio->ret);
	struct pbuf *pbuf = aio->arg;
	const uint8_t *buf = (const uint8_t *)pbuf->payload;

	if (buf[12] == 0x08 && buf[13] == 0x00 && // IP
	    buf[23] == 17 && // UDP
	    buf[36] == 7 && buf[37] == 211) // port 2003
	{
		packet_tx_status = status->wifi_tx_result;
	}
}
#endif

esp_err_t graphite(const char *prefix, const char **metric, float *value)
{
        if (sock < 0) {
                esp_err_t err = graphite_init();
                if (err != ESP_OK)
                        return err;
        }

        int msglen = 0;
        char *msg = format(prefix, metric, value, &msglen);
        if (msg == NULL)
                return ESP_FAIL;

// call below relies on the following change to SDK
#if 0
diff --git a/components/lwip/port/esp8266/netif/wlanif.c b/components/lwip/port/esp8266/netif/wlanif.c
index c301ee81..6867d852 100644
--- a/components/lwip/port/esp8266/netif/wlanif.c
+++ b/components/lwip/port/esp8266/netif/wlanif.c
@@ -355,10 +355,17 @@ static void low_level_init(struct netif* netif)
  *
  * @return 0 meaning successs
  */
+
+
+void (* volatile low_level_send_callback)(const esp_aio_t*) = NULL;
+
 static int low_level_send_cb(esp_aio_t* aio)
 {
     struct pbuf* pbuf = aio->arg;

+    if (low_level_send_callback)
+       low_level_send_callback(aio);
+
#endif

#ifdef CONFIG_IDF_TARGET_ESP8266
        extern void (* volatile low_level_send_callback)(const esp_aio_t*);
	low_level_send_callback = track_packet_status;
        for (char i = 0; i < 3; i++) {
                packet_tx_status = -1;
                int n = sendto(sock, msg, msglen, 0, (struct sockaddr *)&addr, sizeof addr);
                if (n != msglen) {
                        vTaskDelay(150 / portTICK_PERIOD_MS);
                        continue;
                }
                for (char j = 0; j < 64; j++) {
                        vTaskDelay(50 / portTICK_PERIOD_MS);
                        if (packet_tx_status != -1)
                                break;
                }
                if (packet_tx_status == TX_STATUS_SUCCESS)
                        break;
        }
        low_level_send_callback = NULL;
#else
        for (char i = 0; i < 3; i++) {
                int n = sendto(sock, msg, msglen, 0, (struct sockaddr *)&addr, sizeof addr);
                if (n == msglen)
                        break;
                vTaskDelay(150 / portTICK_PERIOD_MS);
        }
#endif
        free(msg);

        return ESP_OK;
}
