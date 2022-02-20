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
#include "wifi.h"

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

static int trim_color_escape_seq_and_newline(char *msg, int len)
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
                if (r[0] == '\n') {
                        r++;
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

static void buffer_send(char *msg, int len)
{
        /* printf("{{{%.*s}}}\n", len, msg); */

        len = trim_color_escape_seq_and_newline(msg, len);
        msg[len] = 0;

        // Ignore garbage produced by SDK
        // "wifi E (238) timer:0x3ffe9a24 cb is null" messages
        // W (787) wifi:<ba-add>idx:1 (ifx:0, 4c:ed:fb:b2:df:a8), tid:0, ssn:0, winSize:64
        // W (817) wifi:<ba-del>idx
        // W (817) wifi:hmac tx: ifx0 stop, discard
        if (strstr(msg, "wifi") && (strstr(msg, "cb is null")))
                return;
        if (strstr(msg, "wifi:<ba-add>"))
                return;
        if (strstr(msg, "wifi:<ba-del>"))
                return;
        if (strstr(msg, "wifi:hmac") && strstr(msg, "stop, discard"))
                return;
        if (len == 0)
                return;

        bool taken = xSemaphoreTake(lock, portMAX_DELAY);
        assert(taken == true);
        xMessageBufferSend(msgbuf, msg, len < SIZE ? len : SIZE, 100);
        xSemaphoreGive(lock);
}

#if defined(CONFIG_IDF_TARGET_ESP8266)
static int syslog_putchar(int ch)
{
        static char buf[SIZE], *wptr = buf;

        if (wptr - buf < SIZE)
                *wptr++ = ch;

        if (ch == '\n') {
                buffer_send(buf, wptr - buf);
                wptr = buf;
        }

        if (old_putchar != NULL)
                return old_putchar(ch);
        return ch;
}
#elif defined(CONFIG_IDF_TARGET_ESP32)
static int syslog_vprintf(const char *fmt, va_list va)
{
        static char buf[SIZE];
        static int len;
        int n = vsnprintf(buf + len, SIZE - len, fmt, va);
        if (n <= 0)
                return n;
        len += n;
        if (len >= SIZE)
                len = SIZE;
        if (len == SIZE || buf[len - 1] == '\n' || buf[len - 1] == '\r') {
                buffer_send(buf, len);
                len = 0;
        }

        if (old_vprintf != NULL)
                return old_vprintf(fmt, va);
        return n;
}
#endif

char syslog_last_err[32];
static void syslog_task(void *arg)
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

        while (sock < 0) {
                if (xMessageBufferIsFull(msgbuf))
                        xMessageBufferReceive(msgbuf, msg, sizeof msg - 1, portMAX_DELAY);
                vTaskDelay(100 / portTICK_PERIOD_MS);
        }

        while (1) {
                size_t len = xMessageBufferReceive(msgbuf, msg, sizeof msg - 1, portMAX_DELAY);
                assert(len != 0);

                int header_len = 0;
                unsigned tick;
                int n = sscanf(msg, "%*[EWIDV] (%u) %15[^:]: %n", &tick, tag, &header_len);

                if (n == 2) {
                        int prio = decode_prio(*msg);
                        iov[0].iov_len = snprintf(header, sizeof header,
                                                  "<%d> %s %c (%u) ",
                                                  facility * 8 + prio, tag, *msg, tick);
                        if (prio <= 4) {
                                int errlen = len - header_len;
                                if (errlen >= sizeof syslog_last_err)
                                        errlen = sizeof syslog_last_err - 1;
                                char *err = msg + header_len;
                                if (errlen > 10 && memcmp(err, "prev_err: ", 10) != 0) {
                                        memcpy(syslog_last_err, msg + header_len, errlen);
                                        syslog_last_err[errlen] = 0;
                                }
                        }
                } else {
                        iov[0].iov_len = snprintf(header, sizeof header, "<%d> ", facility * 8 + 6);
                }
                if (iov[0].iov_len >= sizeof header)
                        iov[0].iov_len = sizeof header;

                iov[1].iov_base = msg + header_len;
                iov[1].iov_len = len - header_len;

                while (!wifi_connected() || sendmsg(sock, &msghdr, 0) == -1)
                        vTaskDelay(100 / portTICK_PERIOD_MS);
        }
}

void syslog_early_init()
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

        xTaskCreate(&syslog_task, "log", 3072, NULL, 5, NULL);
#if defined(CONFIG_IDF_TARGET_ESP8266)
        old_putchar = esp_log_set_putchar(syslog_putchar);
#elif defined(CONFIG_IDF_TARGET_ESP32)
        old_vprintf = esp_log_set_vprintf(syslog_vprintf);
#endif


}

void syslog_init()
{
        if (msgbuf == NULL) {
                syslog_early_init();
                if (msgbuf == NULL)
                        return;
        }

        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) {
                ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
                return;
        }
}
