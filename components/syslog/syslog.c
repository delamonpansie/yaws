#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/message_buffer.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "syslog.h"

static const char* TAG = "yaws-syslog";

#if defined(CONFIG_IDF_TARGET_ESP8266)
static putchar_like_t old_putchar;
#elif defined(CONFIG_IDF_TARGET_ESP32)
static vprintf_like_t old_vprintf;
#endif

static int sock = -1;
static SemaphoreHandle_t lock;
static MessageBufferHandle_t msgbuf;

const int facility = CONFIG_SYSLOG_FACILITY;

#define SIZE 512

int trim_color_escape_seq(char *msg, int len)
{
        char *w = msg, *r = msg, *end = msg + len;
        while (r < end) {
                if (end - r >= 6 &&
                    r[0] == '\033' &&
                    r[1] == '[' &&
                    r[3] == ';' &&
                    r[6] == 'm') {
                        r += 7;
                        continue;
                }
                if (end - r >= 4 &&
                    r[0] == '\033' &&
                    r[1] == '[' &&
                    r[2] == '0' &&
                    r[3] == 'm') {
                        r += 4;
                        continue;
                }
                *w++ = *r++;
        }
        return w - msg;
}

static int decode_prio(char ch)
{
        switch (ch) {
        case 'E': return 3;
        case 'W': return 4;
        case 'I': return 5;
        case 'D': return 6;
        case 'V': return 7;
        default: return 7;
        }
}

#if defined(CONFIG_IDF_TARGET_ESP8266)
static int log_putchar(int ch)
{
        static char buf[SIZE], *wptr = buf;

        if (wptr - buf < SIZE)
                *wptr++ = ch;

        if (ch == '\n') {
                bool taken = xSemaphoreTake(lock, portMAX_DELAY);
                assert(taken == true);
                xMessageBufferSend(msgbuf, buf, wptr - buf, 0);
                xSemaphoreGive(lock);
                wptr = buf;
        }

        if (old_putchar != NULL)
                return old_putchar(ch);
        return 0;
}
#elif defined(CONFIG_IDF_TARGET_ESP32)
static int log_vprintf(const char *fmt, va_list va)
{
        char *buf;
        int len = vasprintf(&buf, fmt, va);
        if (len != -1) {
                bool taken = xSemaphoreTake(lock, portMAX_DELAY);
                assert(taken == true);
                xMessageBufferSend(msgbuf, buf, len < SIZE ? len : SIZE, 100);
                xSemaphoreGive(lock);
                free(buf);
        }

        if (old_vprintf != NULL)
                return old_vprintf(fmt, va);
        return 0;
}
#endif

void log_task(void *arg)
{
        char msg[SIZE+1], tag[32], header[64];

        struct sockaddr_in addr = {
                .sin_family = AF_INET,
                .sin_addr = (struct in_addr){
                        .s_addr = inet_addr(CONFIG_SYSLOG_ADDR),
                },
                .sin_port = htons(CONFIG_SYSLOG_PORT)
        };
        struct iovec iov[2] = {
                { .iov_base = header,
                  .iov_len = 1 },
        };
        struct msghdr msghdr = {
                .msg_name = &addr,
                .msg_namelen = sizeof addr,
                .msg_iov = iov,
                .msg_iovlen = 2
        };

        while (1) {
                size_t len = xMessageBufferReceive(msgbuf, msg, sizeof msg - 1, portMAX_DELAY);
                assert(len != 0);

                len = trim_color_escape_seq(msg, len);
                msg[len] = 0;

                int header_len = 0;
                unsigned tick;
                int n = sscanf(msg, "%*[EWIDV] (%u) %15[^:]: %n", &tick, tag, &header_len);

                if (n == 2) {
                        int prio = decode_prio(*msg);
                        iov[0].iov_len = snprintf(header, sizeof header,
                                                  "<%d> %s %c (%u) ",
                                                  facility * 8 + prio, tag, *msg, tick);
                } else {
                        iov[0].iov_len = snprintf(header, sizeof header, "<%d> ", facility * 8 + 6);
                }
                if (iov[0].iov_len >= sizeof header)
                        iov[0].iov_len = sizeof header;

                iov[1].iov_base = msg + header_len;
                iov[1].iov_len = len - header_len;

                while (sock < 0 || sendmsg(sock, &msghdr, 0) == -1) {
                        if (xMessageBufferIsFull(msgbuf))
                                break;
                        vTaskDelay(100 / portTICK_PERIOD_MS);
                }
        }
}

void log_early_init()
{
        if (strlen(CONFIG_SYSLOG_ADDR) == 0) {
                ESP_LOGE(TAG, "syslog address is not configured, logging is disabled");
                return;
        }

        lock = xSemaphoreCreateMutex();
        if (lock == NULL) {
                ESP_LOGE(TAG, "Unable to create semaphore");
                return;
        }

        msgbuf = xMessageBufferCreate(4096);
        if (msgbuf == NULL) {
                ESP_LOGE(TAG, "Unable to create message buffer");
                return;
        }

        xTaskCreate(&log_task, "log", 8192, NULL, 5, NULL);
#if defined(CONFIG_IDF_TARGET_ESP8266)
        old_putchar = esp_log_set_putchar(log_putchar);
#elif defined(CONFIG_IDF_TARGET_ESP32)
        old_vprintf = esp_log_set_vprintf(log_vprintf);
#endif


}

void log_init()
{
        if (msgbuf == NULL) {
                log_early_init();
                if (msgbuf == NULL)
                        return;
        }

        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) {
                ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
                return;
        }
}
