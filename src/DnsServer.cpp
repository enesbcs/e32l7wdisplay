// Captive-portal DNS server (port of the ESP-IDF captive_portal example,
// simplified to a single fixed answer IP: the AP's own address).
#include "DnsServer.hpp"

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include <cstring>
#include <cstdlib>

#define TAG "dns_captive"

#define DNS_PORT_STATIC 53
#define DNS_MAX_LEN 512
#define DNS_TTL_SEC 300

#define OPCODE_MASK 0x7800
#define QR_FLAG (1 << 7)
#define QD_TYPE_A 0x0001

struct __attribute__((packed)) dns_header_t {
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;
    uint16_t an_count;
    uint16_t ns_count;
    uint16_t ar_count;
};

struct __attribute__((packed)) dns_question_tail_t {
    uint16_t type;
    uint16_t cls;
};

struct __attribute__((packed)) dns_answer_t {
    uint16_t ptr_offset;
    uint16_t type;
    uint16_t cls;
    uint32_t ttl;
    uint16_t addr_len;
    uint32_t ip_addr;
};

static TaskHandle_t s_task = nullptr;
static volatile bool s_running = false;

// Walk the compression-free name in [p, end), return pointer just past
// it, or nullptr on overrun/truncation/compression pointer (0xC0). The
// captive reply never needs compression, so anything else is dropped.
static const char *skip_dns_name(const char *p, const char *end) {
    while (p < end) {
        uint8_t n = (uint8_t)*p;
        if (n == 0) return p + 1;
        if (n & 0xC0) return nullptr;
        if (p + 1 + n > end) return nullptr;
        p += 1 + n;
    }
    return nullptr;
}

static int build_reply(const char *req, size_t req_len, char *reply, size_t reply_max, uint32_t ip) {
    if (req_len < (int)sizeof(dns_header_t) || req_len > reply_max) return -1;

    memcpy(reply, req, req_len);
    dns_header_t *hdr = (dns_header_t *)reply;
    if ((htons(hdr->flags) & OPCODE_MASK) != 0) {
        return 0;                       // not a standard query, drop it
    }
    hdr->flags |= htons(QR_FLAG);       // standard response flag
    hdr->flags |= htons(0x0080);        // RA (recursion available -> yes)

    uint16_t qd_count = htons(hdr->qd_count);
    if (qd_count == 0) return 0;

    const char *qd = req + sizeof(dns_header_t);
    int answered = 0;

    for (int i = 0; i < qd_count; i++) {
        if (qd >= req + req_len) break;
        const char *name_end = skip_dns_name(qd, req + req_len);
        if (!name_end) break;
        if (name_end + sizeof(dns_question_tail_t) > req + req_len) break;
        dns_question_tail_t *q = (dns_question_tail_t *)name_end;
        uint16_t qtype = htons(q->type);

        if (qtype == QD_TYPE_A) {
            char *ans = reply + req_len + answered * (int)sizeof(dns_answer_t);
            if (ans + (int)sizeof(dns_answer_t) > reply + reply_max) break;
            dns_answer_t *a = (dns_answer_t *)ans;
            a->ptr_offset = htons((uint16_t)(0xC000 | (qd - req)));  // name = copy of question name
            a->type = htons(QD_TYPE_A);
            a->cls = q->cls;
            a->ttl = htonl(DNS_TTL_SEC);
            a->addr_len = htons(sizeof(uint32_t));
            a->ip_addr = ip;
            answered++;
        }
        qd = name_end + sizeof(dns_question_tail_t);
    }

    hdr->an_count = htons((uint16_t)answered);
    return (int)(req_len + answered * (int)sizeof(dns_answer_t));
}

static void dns_task(void *arg) {
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DNS_PORT_STATIC);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    while (s_running) {
        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) break;
        if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            close(sock);
            break;
        }

        while (s_running) {
            char rx[DNS_MAX_LEN];
            char reply[DNS_MAX_LEN];
            struct sockaddr_in from = {};
            socklen_t from_len = sizeof(from);
            int len = recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr *)&from, &from_len);
            if (len <= 0) continue;

            uint32_t ap_ip = 0;
            esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
            if (ap) {
                esp_netif_ip_info_t info;
                if (esp_netif_get_ip_info(ap, &info) == ESP_OK) ap_ip = info.ip.addr;
            }
            if (ap_ip == 0) ap_ip = ESP_IP4TOADDR(172, 218, 28, 1);

            int rlen = build_reply(rx, (size_t)len, reply, sizeof(reply), ap_ip);
            if (rlen > 0) {
                sendto(sock, reply, (size_t)rlen, 0, (struct sockaddr *)&from, from_len);
            }
        }
        close(sock);
    }
    s_running = false;
    vTaskDelete(nullptr);
}

void dns_captive_start(void) {
    if (s_running && s_task) return;
    s_running = true;
    if (xTaskCreate(dns_task, "dns_captive", 4096, nullptr, 5, &s_task) != pdPASS) {
        s_running = false;
        ESP_LOGE(TAG, "failed to create dns task");
    }
    ESP_LOGI(TAG, "captive portal DNS up");
}

void dns_captive_stop(void) {
    s_running = false;
    if (s_task) {
        vTaskDelete(s_task);
        s_task = nullptr;
    }
}