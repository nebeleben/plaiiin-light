#include "captive_dns.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <errno.h>
#include <stdatomic.h>
#include <string.h>

static const char *TAG = "captive_dns";
#define DNS_PORT 53
#define CAPTIVE_AP_IP 0xC0A80401 /* 192.168.4.1 */

static int s_sock = -1;
static TaskHandle_t s_task;
static atomic_flag s_running = ATOMIC_FLAG_INIT;

/* Walk the QNAME labels starting at offset 12, then skip QTYPE(2)+QCLASS(2).
 * Returns the QTYPE and sets *qend to the offset just past QCLASS (end of
 * the question section), or returns -1 if the packet is truncated/malformed. */
static int parse_question(const uint8_t *buf, int len, int *qend)
{
    int off = 12;
    while (off < len) {
        uint8_t label_len = buf[off];
        if (label_len == 0) { off += 1; break; }
        if ((label_len & 0xC0) != 0) return -1; /* compression not expected in a query */
        off += 1 + label_len;
    }
    if (off + 4 > len) return -1;
    int qtype = (buf[off] << 8) | buf[off + 1];
    *qend = off + 4;
    return qtype;
}

/* Minimal DNS: answer A/ANY queries with one A record -> CAPTIVE_AP_IP;
 * everything else gets a header-only, no-answer response. */
static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    s_sock = sock;
    struct sockaddr_in addr = {
        .sin_family = AF_INET, .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind :%d failed (errno=%d); aborting captive_dns task", DNS_PORT, errno);
        close(sock);
        s_sock = -1;
        s_task = NULL;
        atomic_flag_clear(&s_running);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "captive DNS listening on :%d", DNS_PORT);

    uint8_t buf[512];
    while (1) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int len = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&src, &slen);
        if (len < 0) {
            /* A persistent socket error (e.g. errno=EBADF/ENOTCONN in the
             * window where captive_dns_stop() has already closed s_sock but
             * hasn't yet reached vTaskDelete(s_task)) must not busy-spin:
             * this task runs at priority 5, and a tight loop here can starve
             * IDLE and trip the task watchdog. Log and back off with a short
             * delay and retry -- a stop() in progress remains free to
             * vTaskDelete(s_task) out from under this delay at any time
             * (FreeRTOS allows deleting a blocked/delayed task). */
            ESP_LOGW(TAG, "recvfrom failed (errno=%d); backing off", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (len < 12) continue;

        int qend = 0;
        int qtype = parse_question(buf, len, &qend);

        buf[2] = 0x81; buf[3] = 0x80; /* response, no error */

        if (qtype < 0 || (qtype != 1 /* A */ && qtype != 255 /* ANY */)) {
            /* Header-only response: drop question and answer sections. */
            buf[4] = buf[5] = 0;                     /* QDCOUNT = 0 */
            buf[6] = buf[7] = 0;                     /* ANCOUNT = 0 */
            buf[8] = buf[9] = buf[10] = buf[11] = 0;  /* NS/AR = 0 */
            sendto(sock, buf, 12, 0, (struct sockaddr *)&src, slen);
            continue;
        }

        buf[4] = 0x00; buf[5] = 0x01;             /* QDCOUNT = 1 */
        buf[6] = 0x00; buf[7] = 0x01;             /* ANCOUNT = 1 */
        buf[8] = buf[9] = buf[10] = buf[11] = 0;  /* NS/AR = 0 */

        uint8_t answer[] = {
            0xC0, 0x0C,             /* name -> pointer to the query name at offset 12 */
            0x00, 0x01, 0x00, 0x01, /* TYPE A, CLASS IN */
            0x00, 0x00, 0x00, 0x3C, /* TTL 60s */
            0x00, 0x04,             /* RDLENGTH 4 */
            (CAPTIVE_AP_IP >> 24) & 0xFF, (CAPTIVE_AP_IP >> 16) & 0xFF,
            (CAPTIVE_AP_IP >> 8) & 0xFF, CAPTIVE_AP_IP & 0xFF,
        };
        memcpy(buf + qend, answer, sizeof(answer));
        sendto(sock, buf, qend + sizeof(answer), 0, (struct sockaddr *)&src, slen);
    }
}

void captive_dns_start(void)
{
    if (atomic_flag_test_and_set(&s_running)) return; /* already running/starting */
    xTaskCreate(dns_task, "captive_dns", 3072, NULL, 5, &s_task);
    ESP_LOGI(TAG, "captive DNS starting");
}

void captive_dns_stop(void)
{
    if (!s_task) return;
    if (s_sock >= 0) {
        close(s_sock); /* unblocks the recvfrom() in dns_task */
        s_sock = -1;
    }
    vTaskDelete(s_task);
    s_task = NULL;
    atomic_flag_clear(&s_running);
    ESP_LOGI(TAG, "captive DNS stopped");
}
