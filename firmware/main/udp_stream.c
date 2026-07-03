/*
 * UDP transport — sends encoded audio packets to a receiver via a standard
 * UDP socket (lwIP). Requires WiFi AP association (router).
 *
 * Independent module — does not share state with tcp_stream.c or
 * rawtx_stream.c. Raw 802.11 TX is in rawtx_stream.c.
 */

/* ---- System / SDK includes ---- */
#include <string.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "lwip/sockets.h"
#include "esp_log.h"

/* ---- Project includes ---- */
#include "board_config.h"
#include "udp_stream.h"

#ifndef IPTOS_DSCP_EF
#define IPTOS_DSCP_EF 0xB8
#endif

static const char *TAG = "udp";

static int s_sock = -1;
static struct sockaddr_in s_dest;
static bool s_ready = false;

esp_err_t udp_stream_init(uint32_t host_ip, uint16_t host_port)
{
    if (s_ready)
    {
        udp_stream_deinit();
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0)
    {
        ESP_LOGE(TAG, "socket: errno=%d", errno);
        return ESP_FAIL;
    }

    struct timeval tv = {
        .tv_sec = UDP_SEND_TIMEOUT_MS / 1000,
        .tv_usec = (UDP_SEND_TIMEOUT_MS % 1000) * 1000,
    };
    setsockopt(s_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    // Configure client socket: optional

    /* Set TOS (Type of Service) to Expedited Forwarding (Voice). */
    //int tos = IPTOS_DSCP_EF;
    //setsockopt(s_sock, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));

    memset(&s_dest, 0, sizeof(s_dest));
    s_dest.sin_family = AF_INET;
    s_dest.sin_port = htons(host_port);
    s_dest.sin_addr.s_addr = host_ip; /* already network byte order */

    ESP_LOGI(TAG, "UDP -> %d.%d.%d.%d:%u",
             (int)(host_ip & 0xFF), (int)((host_ip >> 8) & 0xFF),
             (int)((host_ip >> 16) & 0xFF), (int)((host_ip >> 24) & 0xFF),
             (unsigned)host_port);

    s_ready = true;
    return ESP_OK;
}

esp_err_t udp_stream_deinit(void)
{
    if (s_sock >= 0)
    {
        close(s_sock);
        s_sock = -1;
    }
    s_ready = false;
    return ESP_OK;
}

bool udp_stream_is_ready(void)
{
    return s_ready;
}

esp_err_t udp_stream_send(const uint8_t *data, size_t len)
{
    if (!data || !len || !s_ready)
        return ESP_ERR_INVALID_ARG;

    if (sendto(s_sock, data, len, 0,
               (struct sockaddr *)&s_dest, sizeof(s_dest)) < 0)
        return ESP_FAIL;
    return ESP_OK;
}
