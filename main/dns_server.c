// Simple DNS Hijack Server for ESP32 Captive Portal
// Responds to all queries with the AP IP (default: 192.168.4.1)

#include "dns_server.h"
#include <string.h>
#include <sys/param.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <lwip/inet.h>
#include <esp_log.h>
#include "commonutils.h"

// External task function defined in tasks.c
extern void dns_server_create_task(void);

#define DNS_PORT 53
#define DNS_MAX_PACKET_SIZE 512
static const char *TAG = "DNSHijack";

static uint8_t dns_response[DNS_MAX_PACKET_SIZE];
static int dns_sock = -1;
static struct sockaddr_in dns_addr;
static char ap_ip_str[16] = "192.168.4.1";

void dns_server_set_ap_ip(const char *ip)
{
    strncpy(ap_ip_str, ip, sizeof(ap_ip_str));
    ap_ip_str[sizeof(ap_ip_str) - 1] = '\0';
}

void dns_server_start(const char *ap_ip)
{
    HEAP_TRACE_START("DNS_START");

    if (ap_ip)
        dns_server_set_ap_ip(ap_ip);

    // Create DNS server task (centralized function, local task)
    dns_server_create_task();

    HEAP_TRACE_END(200); // Task creation allocates memory for task stack and TCB
}

void dns_server_run(void)
{
    HEAP_TRACE_START("DNS_TASK");

    dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (dns_sock < 0)
    {
        ESP_LOGE(TAG, "Failed to create DNS socket");
        return;
    }
    memset(&dns_addr, 0, sizeof(dns_addr));
    dns_addr.sin_family = AF_INET;
    dns_addr.sin_port = htons(DNS_PORT);
    dns_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(dns_sock, (struct sockaddr *)&dns_addr, sizeof(dns_addr)) < 0)
    {
        ESP_LOGE(TAG, "Failed to bind DNS socket");
        close(dns_sock);
        return;
    }
    ESP_LOGI(TAG, "DNS hijack server started");

    HEAP_TRACE_END(100); // DNS socket and task setup may allocate memory

    while (1)
    {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int len = recvfrom(dns_sock, dns_response, DNS_MAX_PACKET_SIZE, 0,
                           (struct sockaddr *)&client_addr, &addr_len);
        if (len > 0)
        {
            // Minimal DNS response: copy header, set response flags, answer with AP IP
            if (len < 12)
                continue;            // DNS header is 12 bytes
            dns_response[2] |= 0x80; // QR=1 (response)
            dns_response[3] |= 0x80; // RA=1
            dns_response[7] = 1;     // ANCOUNT = 1
            // Append answer: type A, class IN, TTL, RDLENGTH, RDATA
            int resp_len = len;
            // Skip to end of query
            int i = 12;
            while (i < len && dns_response[i] != 0)
                i++;
            i += 5; // null + QTYPE + QCLASS
            if (i + 16 > DNS_MAX_PACKET_SIZE)
                continue;
            memcpy(&dns_response[i], dns_response + 12, i - 12); // Name
            dns_response[i + 0] = 0xc0;
            dns_response[i + 1] = 0x0c; // Name ptr
            dns_response[i + 2] = 0x00;
            dns_response[i + 3] = 0x01; // Type A
            dns_response[i + 4] = 0x00;
            dns_response[i + 5] = 0x01; // Class IN
            dns_response[i + 6] = 0x00;
            dns_response[i + 7] = 0x00;
            dns_response[i + 8] = 0x00;
            dns_response[i + 9] = 0x3c; // TTL 60s
            dns_response[i + 10] = 0x00;
            dns_response[i + 11] = 0x04; // RDLENGTH 4
            struct in_addr ap_ip;
            inet_aton(ap_ip_str, &ap_ip);
            memcpy(&dns_response[i + 12], &ap_ip, 4); // RDATA
            resp_len = i + 16;
            sendto(dns_sock, dns_response, resp_len, 0,
                   (struct sockaddr *)&client_addr, addr_len);
        }
    }
}

void dns_server_stop(void)
{
    if (dns_sock >= 0)
        close(dns_sock);
}
