/*
 * ESP8266 WiFi Microphone - Main Application
 *
 * Architecture:
 *   Service Port (always active) for discovery/control
 *   I2S Task -> PCM Queue -> ADPCM Task -> ADPCM Queue -> UDP Task
 *
 * Stream control via FreeRTOS EventGroup:
 *   STREAM_EVT_START_REQ  - set by svc_port on CONFIGURE
 *   STREAM_EVT_STOP_REQ   - set by svc_port on watchdog expiry / re-CONFIGURE
 *   STREAM_EVT_ACTIVE     - set by start_streaming, cleared by stop_streaming
 *
 * Clean shutdown: pipeline tasks exit their loops when STREAM_EVT_ACTIVE is
 * cleared, give per-task done semaphores, and self-delete. The I2S task uses
 * a short i2s_read timeout (computed from DMA buffer capacity) so it re-checks
 * the active flag frequently - enables fast clean stop without force-deletion
 * (which would leave the I2S driver mutex locked and deadlock i2s_driver_uninstall).
 */

/* ---- System / SDK includes ---- */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"

/* ---- Project includes ---- */
#include "board_config.h"
#include "config_mgr.h"
#include "wifi_sta.h"
#include "svc_port.h"
#include "svc_protocol.h"
#include "i2s_capture.h"
#include "adpcm_encoder.h"
#include "tpdf_dither.h"
#include "packet_format.h"
#include "at_cmd.h"
#include "battery.h"
#include "stream_mode.h"
#include "stream_control.h"

static const char *TAG = "main";

/* ---- Frame types with flexible array members ---- */

typedef struct
{
    int num_samples;
    int16_t samples[];
} pcm_frame_t;

typedef struct
{
    uint16_t data_len;
    uint16_t seq_num;
    uint32_t timestamp_ms;
    uint8_t data[];
} adpcm_frame_t;

/* ---- Pipeline task indices ---- */

typedef enum
{
    TASK_IDX_I2S = 0,
    TASK_IDX_ADPCM = 1,
    TASK_IDX_UDP = 2,
    TASK_IDX_COUNT = 3
} task_idx_t;

/* ---- Queues & pools ---- */

static QueueHandle_t pcm_free_queue = NULL;
static QueueHandle_t pcm_filled_queue = NULL;
static QueueHandle_t adpcm_free_queue = NULL;
static QueueHandle_t adpcm_filled_queue = NULL;

static adpcm_enc_state_t *adpcm_enc[2] = {NULL, NULL};

/* ---- Runtime audio parameters (set at stream start) ---- */

static uint8_t s_channels = AUDIO_CHANNELS;
static uint32_t s_sample_rate = 0;
static int s_samples_per_frame = 0;
static int s_adpcm_frame_bytes = 0;
static uint32_t s_audio_bitrate = 0;
static uint8_t s_sample_rate_enum = 0;
static uint8_t s_codec_mode = CODEC_MODE_ADPCM;
static uint8_t s_pkt_codec_id = CODEC_ID_ADPCM;
static int s_pkt_data_len = 0;     /* bytes of payload per packet */
static int s_bits_per_sample = 16; /* I2S bit depth (16 or 24) */

/* ---- Task handles & done semaphores ---- */

static TaskHandle_t s_task_handles[TASK_IDX_COUNT] = {NULL};
static SemaphoreHandle_t s_task_done_sems[TASK_IDX_COUNT] = {NULL};

/* ---- Stream control EventGroup ---- */

static EventGroupHandle_t s_stream_evt_grp = NULL;
static bool s_wifi_initialized = false;

/* Pool sizes - computed at start_streaming from free heap & frame_ms. */
static int s_pcm_pool_size = 4;
static int s_adpcm_pool_size = 6;

/* Frame duration (ms) - computed in start_streaming from I2S params. */
static uint32_t s_frame_ms = 20;

/* Accessor for other modules (svc_port, at_cmd) - returns current frame_ms. */
uint32_t streaming_get_frame_ms(void) { return s_frame_ms; }

/* ---- Stream control API (for AT commands) ---- */

bool streaming_is_active(void)
{
    if (!s_stream_evt_grp)
        return false;
    return (xEventGroupGetBits(s_stream_evt_grp) & STREAM_EVT_ACTIVE) != 0;
}

bool streaming_request_stop(void)
{
    if (!s_stream_evt_grp)
        return false;
    xEventGroupSetBits(s_stream_evt_grp, STREAM_EVT_STOP_REQ);
    return true;
}

bool streaming_request_restart(void)
{
    if (!s_stream_evt_grp)
        return false;
    /* Only restart if currently streaming. If the stream is already stopped
     * (e.g., after CMD_STOP from server), don't auto-start - the saved NVS
     * params will apply naturally on the next stream start. This prevents
     * HOTRESTART from overriding an intentional stop. */
    if (!streaming_is_active())
        return false;
    /* Set STOP first, then START. Main loop processes bits in order:
     * STOP_REQ -> stop_streaming(), then START_REQ -> start_streaming().
     * This gives a clean restart with fresh NVS config (~200ms vs ~1s reboot). */
    xEventGroupSetBits(s_stream_evt_grp, STREAM_EVT_STOP_REQ);
    xEventGroupSetBits(s_stream_evt_grp, STREAM_EVT_START_REQ);
    return true;
}

/* ---- Helpers ---- */

static pcm_frame_t *pcm_frame_alloc(int num_samples, int bits_per_sample, int codec_mode)
{
    /* Only PCM 24-bit mode needs int32_t (raw sign-extended 24-bit values
     * copied directly from i2s_capture_read). ADPCM 24-bit uses int16_t
     * because dither_buffer_24_to_16 reduces to 16-bit before encoding.
     * Allocating int32_t for ADPCM wastes 19 KB at 48kHz/stereo. */
    bool need_int32 = (bits_per_sample == 24) && (codec_mode == CODEC_MODE_PCM);
    size_t elem_size = need_int32 ? sizeof(int32_t) : sizeof(int16_t);
    pcm_frame_t *f = malloc(sizeof(pcm_frame_t) + (size_t)num_samples * elem_size);
    if (f)
        f->num_samples = num_samples;
    return f;
}

static adpcm_frame_t *adpcm_frame_alloc(int max_data_len)
{
    adpcm_frame_t *f = malloc(sizeof(adpcm_frame_t) + max_data_len);
    return f;
}

static void drain_and_delete_queue(QueueHandle_t *q)
{
    if (!*q)
        return;
    void *item = NULL;
    while (xQueueReceive(*q, &item, 0) == pdTRUE)
    {
        free(item);
    }
    vQueueDelete(*q);
    *q = NULL;
}

/* ====================================================================
 * I2S Capture Task
 * ==================================================================== */

static void i2s_task_fn(void *arg)
{
    int idx = (int)arg;
    ESP_LOGI(TAG, "[I2S] Task started (idx=%d)", idx);

    int total = s_samples_per_frame * s_channels;
    int32_t *raw = malloc(total * sizeof(int32_t));
    if (!raw)
    {
        ESP_LOGE(TAG, "[I2S] alloc fail");
        svc_port_set_error(SVC_ERR_MEMORY);
        goto task_exit;
    }

    bool is_24bit = (i2s_capture_get_bits() == 24);
    uint32_t pcm_wait_ms = (uint32_t)s_frame_ms * (s_pcm_pool_size + 2);

    /* Diagnostic counters - log every 50 frames or on timeout. */
    uint32_t ok_count = 0;
    uint32_t timeout_count = 0;

    while (streaming_is_active())
    {
        pcm_frame_t *pcm = NULL;
        if (xQueueReceive(pcm_free_queue, &pcm, pdMS_TO_TICKS(pcm_wait_ms)) != pdTRUE)
            continue;

        int n = 0;
        esp_err_t err = i2s_capture_read(raw, total, &n);
        if (err != ESP_OK)
        {
            timeout_count++;
            if (err != ESP_ERR_TIMEOUT)
            {
                ESP_LOGE(TAG, "[I2S] read error: %s", esp_err_to_name(err));
                svc_port_set_error(SVC_ERR_I2S);
            }
            /* Log first timeout and every 50th - shows if I2S is starved. */
            if (timeout_count == 1 || (timeout_count % 50) == 0)
            {
                ESP_LOGW(TAG, "[I2S] read timeout #%u (ok=%u) - no DMA data?",
                         (unsigned)timeout_count, (unsigned)ok_count);
            }
            xQueueSend(pcm_free_queue, &pcm, 0);
            continue;
        }

        ok_count++;
        /* Log first successful read + every 1000 - shows pipeline is alive. */
        if (ok_count == 1)
        {
            ESP_LOGI(TAG, "[I2S] first read OK: %d samples, raw[0]=%d, raw[1]=%d",
                     n, (int)raw[0], (int)raw[1]);
        }
        else if ((ok_count % 1000) == 0)
        {
            ESP_LOGI(TAG, "[I2S] %u frames read (timeouts=%u)",
                     (unsigned)ok_count, (unsigned)timeout_count);
        }

        for (int i = n; i < total; i++)
            raw[i] = 0;

        if (s_codec_mode == CODEC_MODE_PCM && is_24bit)
        {
            /* PCM 24-bit: copy raw int32 samples directly (sign-extended
             * 24-bit values). pcm_task_fn will strip the high byte and
             * emit 3 bytes per sample. No dither - we want full 24-bit
             * precision in the stream. */
            memcpy(pcm->samples, raw, (size_t)total * sizeof(int32_t));
        }
        else if (is_24bit)
            dither_buffer_24_to_16(raw, pcm->samples, total);
        else
            dither_buffer_passthrough(raw, pcm->samples, total);

        if (xQueueSend(pcm_filled_queue, &pcm, 0) != pdTRUE)
            xQueueSend(pcm_free_queue, &pcm, 0);
    }

    free(raw);

task_exit:
    ESP_LOGI(TAG, "[I2S] Task exiting");
    if (s_task_done_sems[idx])
        xSemaphoreGive(s_task_done_sems[idx]);
    vTaskDelete(NULL);
}

/* ====================================================================
 * ADPCM Encoding Task
 * ==================================================================== */

static void adpcm_task_fn(void *arg)
{
    int idx = (int)arg;
    ESP_LOGI(TAG, "[ADPCM] Task started (idx=%d, %d ch)", idx, s_channels);

    uint32_t frame_count = 0;
    uint16_t seq_counter = 0;
    int cap = DVI4_HEADER_SIZE + s_adpcm_frame_bytes;

    int16_t *ch_left = NULL, *ch_right = NULL;
    if (s_channels == 2)
    {
        ch_left = malloc(s_samples_per_frame * sizeof(int16_t));
        ch_right = malloc(s_samples_per_frame * sizeof(int16_t));
        if (!ch_left || !ch_right)
        {
            ESP_LOGE(TAG, "[ADPCM] deinterleave alloc fail");
            free(ch_left);
            free(ch_right);
            goto task_exit;
        }
    }

    while (streaming_is_active())
    {
        pcm_frame_t *pcm = NULL;
        adpcm_frame_t *adpcm = NULL;

        if (xQueueReceive(pcm_filled_queue, &pcm, pdMS_TO_TICKS(100)) != pdTRUE)
            continue;
        if (xQueueReceive(adpcm_free_queue, &adpcm, pdMS_TO_TICKS(100)) != pdTRUE)
        {
            xQueueSend(pcm_free_queue, &pcm, 0);
            continue;
        }

        size_t written = 0;
        esp_err_t err;

        if (s_channels == 1)
        {
            err = adpcm_enc_process(adpcm_enc[0], pcm->samples, pcm->num_samples,
                                    adpcm->data, cap, &written);
        }
        else
        {
            /* Stereo: deinterleave L,R,L,R -> ch_left[], ch_right[] */
            for (int i = 0; i < s_samples_per_frame; i++)
            {
                ch_left[i] = pcm->samples[i * 2];
                ch_right[i] = pcm->samples[i * 2 + 1];
            }
            size_t wl = 0, wr = 0;
            err = adpcm_enc_process(adpcm_enc[0], ch_left, s_samples_per_frame,
                                    adpcm->data, cap, &wl);
            if (err == ESP_OK)
                err = adpcm_enc_process(adpcm_enc[1], ch_right, s_samples_per_frame,
                                        adpcm->data + wl, cap, &wr);
            written = wl + wr;
        }

        if (err != ESP_OK || written > UINT16_MAX)
        {
            if (err != ESP_OK)
                svc_port_set_error(SVC_ERR_CODEC);
            xQueueSend(adpcm_free_queue, &adpcm, 0);
            xQueueSend(pcm_free_queue, &pcm, 0);
            continue;
        }

        adpcm->data_len = (uint16_t)written;
        adpcm->seq_num = seq_counter++;
        adpcm->timestamp_ms = frame_count * s_frame_ms;

        if (xQueueSend(adpcm_filled_queue, &adpcm, 0) != pdTRUE)
            xQueueSend(adpcm_free_queue, &adpcm, 0);
        xQueueSend(pcm_free_queue, &pcm, 0);

        if ((++frame_count % 1000) == 0)
            ESP_LOGI(TAG, "[ADPCM] %" PRIu32 " frames encoded", frame_count);
    }

    free(ch_left);
    free(ch_right);

task_exit:
    ESP_LOGI(TAG, "[ADPCM] Task exiting");
    if (s_task_done_sems[idx])
        xSemaphoreGive(s_task_done_sems[idx]);
    vTaskDelete(NULL);
}

/* ====================================================================
 * PCM Packing Task (alternative to ADPCM when codec_mode == PCM)
 *
 * Packs raw PCM samples (already 16-bit after dither/passthrough) into
 * the packet payload WITHOUT any compression. No DVI4 header per channel.
 *
 * Layout (16-bit):
 *   mono:   [S0][S1][S2]...               (2 bytes/sample)
 *   stereo: [L0][R0][L1][R1]...           (interleaved, 4 bytes/frame)
 *
 * Layout (24-bit - sample occupies 32 bits in int32_t, top byte unused):
 *   mono:   [S0_lo][S0_mid][S0_hi]...                 (3 bytes/sample)
 *   stereo: [L0_lo][L0_mid][L0_hi][R0_lo][R0_mid][R0_hi]... (6 bytes/frame)
 *
 * 24-bit samples are stored in int32_t as sign-extended values in
 * range [-8388608, +8388607]. We emit only the low 3 bytes (little-endian),
 * stripping the redundant high byte.
 * ==================================================================== */
static void pcm_task_fn(void *arg)
{
    int idx = (int)arg;
    ESP_LOGI(TAG, "[PCM] Task started (idx=%d, %d ch, %d-bit)",
             idx, s_channels, s_bits_per_sample);

    uint32_t frame_count = 0;
    uint16_t seq_counter = 0;

    while (streaming_is_active())
    {
        pcm_frame_t *pcm = NULL;
        adpcm_frame_t *out = NULL;

        if (xQueueReceive(pcm_filled_queue, &pcm, pdMS_TO_TICKS(100)) != pdTRUE)
            continue;
        if (!pcm)
            continue;
        if (xQueueReceive(adpcm_free_queue, &out, pdMS_TO_TICKS(100)) != pdTRUE)
        {
            xQueueSend(pcm_free_queue, &pcm, 0);
            continue;
        }
        if (!out)
        {
            xQueueSend(pcm_free_queue, &pcm, 0);
            continue;
        }

        int n = pcm->num_samples; /* = s_samples_per_frame * s_channels */
        uint8_t *dst = out->data;
        size_t written = 0;

        if (s_bits_per_sample == 16)
        {
            /* 16-bit: samples already int16 in pcm->samples[]. Just copy. */
            size_t bytes = (size_t)n * sizeof(int16_t);
            if (bytes > (size_t)s_pkt_data_len)
                bytes = s_pkt_data_len;
            memcpy(dst, pcm->samples, bytes);
            written = bytes;
        }
        else
        {
            /* 24-bit: pcm->samples[] actually holds int32_t (sign-extended
             * 24-bit values, copied raw from i2s_capture_read). Emit only
             * the low 3 bytes (LE) per sample, stripping the redundant
             * high byte. */
            int32_t *s32 = (int32_t *)pcm->samples;
            for (int i = 0; i < n; i++)
            {
                int32_t s = s32[i];
                dst[written++] = (uint8_t)(s & 0xFF);
                dst[written++] = (uint8_t)((s >> 8) & 0xFF);
                dst[written++] = (uint8_t)((s >> 16) & 0xFF);
                if (written + 3 > (size_t)s_pkt_data_len)
                    break;
            }
        }

        out->data_len = (uint16_t)written;
        out->seq_num = seq_counter++;
        out->timestamp_ms = frame_count * s_frame_ms;

        if (xQueueSend(adpcm_filled_queue, &out, 0) != pdTRUE)
            xQueueSend(adpcm_free_queue, &out, 0);
        xQueueSend(pcm_free_queue, &pcm, 0);

        if ((++frame_count % 1000) == 0)
            ESP_LOGI(TAG, "[PCM] %" PRIu32 " frames packed", frame_count);
    }

    ESP_LOGI(TAG, "[PCM] Task exiting");
    if (s_task_done_sems[idx])
        xSemaphoreGive(s_task_done_sems[idx]);
    vTaskDelete(NULL);
}

/* ====================================================================
 * Stream TX Task - sends encoded audio packets via the active transport
 * (UDP socket or Raw 802.11 TX). Mode-agnostic: all mode differences
 * are handled by the stream_mode_ops table.
 * ==================================================================== */

static void stream_task_fn(void *arg)
{
    int idx = (int)arg;
    const stream_mode_ops_t *ops = stream_mode_ops();
    ESP_LOGI(TAG, "[%s] Task started (idx=%d)", ops->name, idx);

    /* Wait for transport ready (+ WiFi association in UDP mode).
     * In Raw TX mode, wifi_sta_is_connected() is not meaningful (no AP),
     * so we only check transport readiness.
     *
     * In UDP mode, without WiFi association, esp_wifi_tx returns ENOMEM
     * (errno=12), causing startup drops. Waiting here eliminates them. */
    int wait_count = 0;
    while (streaming_is_active())
    {
        bool transport_ready = transport_is_ready();
        bool wifi_ready = ops->needs_wifi_association
                              ? wifi_sta_is_connected()
                              : true;
        if (transport_ready && wifi_ready)
            break;

        vTaskDelay(pdMS_TO_TICKS(200));
        if (++wait_count > 150)
        {
            ESP_LOGE(TAG, "[%s] Stream/WiFi not ready after 30s - giving up",
                     ops->name);
            svc_port_set_error(SVC_ERR_NETWORK);
            goto task_exit;
        }
    }

    if (!streaming_is_active())
        goto task_exit;

    ESP_LOGI(TAG, "[%s] Streaming started (WiFi %s)",
             ops->name,
             wifi_sta_is_connected() ? "connected" : "WARN: not connected");

    uint8_t *pkt = malloc(PKT_HDR_SIZE + s_pkt_data_len);
    if (!pkt)
    {
        ESP_LOGE(TAG, "[%s] alloc fail", ops->name);
        svc_port_set_error(SVC_ERR_MEMORY);
        goto task_exit;
    }

    uint32_t sent = 0, dropped = 0;

    while (streaming_is_active())
    {
        adpcm_frame_t *adpcm = NULL;
        if (xQueueReceive(adpcm_filled_queue, &adpcm, pdMS_TO_TICKS(100)) != pdTRUE)
            continue;

        if (!transport_is_ready() || !adpcm->data_len)
        {
            dropped++;
            xQueueSend(adpcm_free_queue, &adpcm, 0);
            continue;
        }

        pkt_header_t hdr;
        /* bits field carries I2S bit depth (16 or 24). For ADPCM, ESP dithers
         * 24->16 before encoding, so the decoded audio is always 16-bit - but
         * we still report the I2S config so the receiver can detect config
         * changes. The receiver's WaveOut format for ADPCM is always 16-bit
         * regardless of this field. */
        pkt_header_init(&hdr, adpcm->seq_num, adpcm->timestamp_ms,
                        s_pkt_codec_id, s_sample_rate_enum, s_channels,
                        s_frame_ms, s_audio_bitrate,
                        (uint16_t)s_bits_per_sample);

        memcpy(pkt, &hdr, PKT_HDR_SIZE);
        memcpy(pkt + PKT_HDR_SIZE, adpcm->data, adpcm->data_len);

        if (transport_send(pkt, PKT_HDR_SIZE + adpcm->data_len) == ESP_OK)
        {
            sent++;
            /* Clear stale NETWORK error after successful send - the initial
             * send may fail (errno=12 ENOMEM while WiFi still associating),
             * but once streaming works the error flag should be cleared so
             * the server status shows "OK" instead of stale "Error". */
            if (dropped > 0 && (sent % 100) == 0)
            {
                svc_port_clear_error();
            }
        }
        else if (++dropped == 1 || (dropped % 100) == 0)
        {
            ESP_LOGW(TAG, "[%s] send fail (drops: %" PRIu32 ")",
                     ops->name, dropped);
            /* Only report network error if stream is still active.
             * During stop, transport_close_client() closes the socket →
             * send() fails with errno=128 (ENOTCONN) — this is expected,
             * not a real error. Reporting it causes STATUS_ERROR in INFO
             * packets, showing "Error" in the receiver UI after stop. */
            if (streaming_is_active())
                svc_port_set_error(SVC_ERR_NETWORK);
        }

        svc_port_update_stats(sent);
        xQueueSend(adpcm_free_queue, &adpcm, 0);

        if (sent && (sent % 1000) == 0)
            ESP_LOGI(TAG, "[%s] %" PRIu32 " sent, %" PRIu32 " dropped",
                     ops->name, sent, dropped);
    }

    free(pkt);

task_exit:
    ESP_LOGI(TAG, "[%s] Task exiting", ops->name);
    if (s_task_done_sems[idx])
    {
        xSemaphoreGive(s_task_done_sems[idx]);
    }
    vTaskDelete(NULL);
}

/* ====================================================================
 * Task exit helper
 * ==================================================================== */

static bool wait_for_task_exit(int idx, uint32_t timeout_ms)
{
    if (!s_task_done_sems[idx])
    {
        return false;
    }

    if (xSemaphoreTake(s_task_done_sems[idx], pdMS_TO_TICKS(timeout_ms)) == pdTRUE)
    {
        s_task_handles[idx] = NULL;
        return true;
    }

    if (s_task_handles[idx])
    {
        ESP_LOGW(TAG, "Task %d did not exit in %u ms - force deleting",
                 idx, (unsigned)timeout_ms);
        vTaskDelete(s_task_handles[idx]);
        s_task_handles[idx] = NULL;
    }
    return false;
}

/* ====================================================================
 * Stream start / stop
 * ==================================================================== */

static esp_err_t start_streaming(void)
{
    if (streaming_is_active())
    {
        /* Duplicate CONFIGURE from server (it sends 3 with 200ms gaps).
         * Not an error - just ignore. */
        return ESP_ERR_INVALID_STATE;
    }

    device_config_t cfg;
    config_get_copy(&cfg);

    /* Re-select transport mode from config. This is CRITICAL for AT+HOTRESTART:
     * AT+XPORT changes cfg.transport_mode in NVS, but s_active_ops was set
     * once at boot. Without this call, switching UDP<->TCP<->RAWTX via
     * AT+XPORT + HOTRESTART has no effect — the old transport stays active.
     *
     * If the transport changed, deinit the old one first (e.g. close TCP
     * listening socket before switching to UDP). */
    const stream_mode_ops_t *old_ops = stream_mode_ops();
    stream_mode_init(&cfg);
    const stream_mode_ops_t *ops = stream_mode_ops();
    if (ops != old_ops)
    {
        ESP_LOGI(TAG, "Transport changed — deinitializing old transport (%s)",
                 old_ops->name);
        old_ops->deinit();
    }

    /* Resolve stream destination (UDP: from server CONFIGURE; RAWTX: none). */
    uint32_t stream_host = 0;
    uint16_t stream_port = 0;
    esp_err_t dest_err = ops->get_stream_dest(&stream_host, &stream_port);
    if (dest_err != ESP_OK)
    {
        ESP_LOGE(TAG, "Cannot resolve stream destination: %s",
                 esp_err_to_name(dest_err));
        return dest_err;
    }

    /* Resolve channel count and notify mode-specific subsystems. */
    s_channels = i2s_capture_channel_count(cfg.channel_format);
    ops->set_channels(s_channels);

    /* Compute runtime audio parameters.
     * frame_ms вычисляется адаптивно от sample_rate и channels - гарантирует:
     *   - чётное samples_per_frame (ADPCM)
     *   - DMA-буфер >= 32 (SDK минимум)
     *   - UDP-пакет <= 1400 байт (MTU) */
    s_sample_rate = cfg.sample_rate;
    s_frame_ms = i2s_capture_compute_frame_ms(s_sample_rate, s_channels,
                                              cfg.codec_mode, cfg.bits_per_sample);
    s_samples_per_frame = (int)(s_sample_rate * s_frame_ms / 1000);

    /* Align samples_per_frame to solve TWO problems at once:
     *
     * 1. SLC word-alignment (ESP8266 SLC передаёт 32-битными словами):
     *    blocksize = dma_buf_len × sample_size должен быть ≡ 0 (mod 4).
     *    Для 16-bit (sample_size=2): dma_buf_len чётный → blocksize ≡ 0 (mod 4).
     *
     * 2. rw_pos drift (want не кратен buf_size → i2s_read читает "хвост"
     *    из следующего буфера, rw_pos растёт, swap-pairs ломается на
     *    нечётном хвосте → теряется 1 сэмпл/кадр → пощипывание):
     *    want = samples × bytes_per_sample, buf_size = dma_buf_len × sample_size.
     *    want кратен buf_size ⇔ samples кратен dma_buf_len ⇔ samples кратен 4
     *    (т.к. dma_buf_len = samples/4).
     *
     * Оба условия: samples кратно 4 И dma_buf_len чётный ⇔ samples кратно 8
     * (для 16-bit). Для 24-bit (sample_size=4) — samples кратно 4.
     *
     * Пример (44100 Гц, 15ms → 661 samples):
     *   БЕЗ выравнивания: samples=661, dl=165→164(фикс), want=1322, buf=328,
     *     rw_pos дрейф 10 байт = 5 сэмплов (НЕЧЁТ!) → потеря 1 сэмпл/кадр
     *   С выравниванием: samples=656, dl=164, want=1312, buf=328,
     *     1312/328=4.0 → rw_pos=0, swap-pairs OK, дрейфа НЕТ */
    if (cfg.bits_per_sample == 16)
        s_samples_per_frame &= ~7;   /* кратно 8: 661→656, 882→880, 220→216 */
    else
        s_samples_per_frame &= ~3;   /* кратно 4 (24-bit) */

    ESP_LOGI(TAG, "Computed frame_ms=%u -> samples_per_frame=%d (rate=%u, ch=%u)",
             (unsigned)s_frame_ms, s_samples_per_frame,
             (unsigned)s_sample_rate, s_channels);

    s_adpcm_frame_bytes = s_samples_per_frame / 2;
    /* Bitrate depends on codec: ADPCM = 4 bits/sample, PCM = bits_per_sample
     * bits/sample. Previously always used 4, showing wrong bitrate for PCM. */
    {
        int bits_per_codec = (s_codec_mode == CODEC_MODE_PCM)
                                 ? s_bits_per_sample : 4;
        s_audio_bitrate = s_sample_rate * (uint32_t)bits_per_codec * s_channels;
    }
    s_sample_rate_enum = sample_rate_to_enum(s_sample_rate);
    s_codec_mode = cfg.codec_mode;
    s_bits_per_sample = cfg.bits_per_sample;

    /* Codec-dependent packet payload size:
     * ADPCM: [DVI4 hdr 4B][adpcm nibbles] per channel -> s_channels x (4 + samples/2)
     * PCM 16-bit: [int16 samples]                       -> samples_per_frame x channels x 2
     * PCM 24-bit: [3 bytes/sample, low bytes of int32]  -> samples_per_frame x channels x 3
     * (24-bit packs to 3 bytes: int32_t sign-extended sample -> strip high byte) */
    int adpcm_data_len, pcm_data_len;
    if (s_codec_mode == CODEC_MODE_PCM)
    {
        int bytes_per_sample = (cfg.bits_per_sample == 24) ? 3 : 2;
        pcm_data_len = s_samples_per_frame * s_channels * bytes_per_sample;
        adpcm_data_len = 0;
        s_pkt_codec_id = CODEC_ID_PCM;
        s_pkt_data_len = pcm_data_len;
    }
    else
    {
        adpcm_data_len = s_channels * (DVI4_HEADER_SIZE + s_adpcm_frame_bytes);
        pcm_data_len = 0;
        s_pkt_codec_id = CODEC_ID_ADPCM;
        s_pkt_data_len = adpcm_data_len;
    }

    /* UDP MTU guard. For PCM at high sample rates, frame_ms is auto-reduced
     * by i2s_capture_compute_frame_ms, but we re-check here. */
    int max_pkt_len = PKT_HDR_SIZE + s_pkt_data_len;
    if (max_pkt_len > 1400)
    {
        ESP_LOGE(TAG, "Packet size %d bytes exceeds UDP MTU (1400). "
                      "Reduce sample rate, frame duration, channels, or use ADPCM. "
                      "(rate=%u, frame=%dms, ch=%u, codec=%s)",
                 max_pkt_len, (unsigned)s_sample_rate, s_frame_ms, s_channels,
                 s_codec_mode == CODEC_MODE_PCM ? "PCM" : "ADPCM");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Audio: %u Hz, %d ms, %u ch, %d samples/frame, codec=%s, %d bytes/pkt, %u bps",
             (unsigned)s_sample_rate, s_frame_ms, s_channels,
             s_samples_per_frame,
             s_codec_mode == CODEC_MODE_PCM ? "PCM" : "ADPCM",
             s_pkt_data_len, (unsigned)s_audio_bitrate);

    /* 1. WiFi - initialize (if not done at boot) and wait for readiness. */
    if (!s_wifi_initialized)
    {
        esp_err_t err = ops->wifi_init(&cfg);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(err));
            return err;
        }
        s_wifi_initialized = true;
    }

    esp_err_t err = ops->wifi_wait_ready(&cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "WiFi not ready: %s", esp_err_to_name(err));
        return err;
    }

    /* 2. Service port (UDP: init EASSP listener; RAWTX: no-op). */
    err = ops->svc_port_init(s_stream_evt_grp, cfg.svc_port);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Service port init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 3. I2S capture. DMA-размеры и таймауты выводятся из samples_per_frame. */
    err = i2s_capture_init(s_sample_rate, cfg.bits_per_sample,
                           cfg.comm_format, cfg.channel_format,
                           s_samples_per_frame, s_frame_ms,
                           cfg.gain, cfg.agc_mode,
                           cfg.i2s_timing_sd_delay,
                           cfg.i2s_timing_ws_delay,
                           cfg.i2s_timing_bck_delay);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "I2S init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 4. TPDF dither. */
    tpdf_init();
    tpdf_seed(esp_random());

    /* 5. ADPCM encoders (one per channel). Skipped in PCM mode. */
    int num_enc = 0;
    if (s_codec_mode == CODEC_MODE_ADPCM)
    {
        num_enc = (s_channels == 2) ? 2 : 1;
    }
    for (int i = 0; i < num_enc; i++)
    {
        adpcm_enc[i] = adpcm_enc_create();
        if (!adpcm_enc[i])
        {
            ESP_LOGE(TAG, "ADPCM encoder %d init failed", i);
            for (int j = 0; j < i; j++)
            {
                adpcm_enc_destroy(adpcm_enc[j]);
                adpcm_enc[j] = NULL;
            }
            i2s_capture_deinit();
            return ESP_FAIL;
        }
    }

    /* 6. Transport init (UDP: create socket; RAWTX: build 802.11 header). */
    err = ops->transport_init(stream_host, stream_port);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Transport init failed: %s", esp_err_to_name(err));
        for (int i = 0; i < num_enc; i++)
        {
            adpcm_enc_destroy(adpcm_enc[i]);
            adpcm_enc[i] = NULL;
        }
        i2s_capture_deinit();
        return err;
    }

    /* 7. Queues & pools - размеры вычисляются адаптивно от free heap и frame_ms.
     *
     * Целевая буферизация: 100мс PCM, 200мс ADPCM.
     *   PCM pool: 100мс - I2S->ADPCM, оба real-time, большой запас не нужен.
     *   ADPCM pool: 200мс - между энкодером (real-time) и UDP/WiFi (jitter).
     *     200мс защищает от WiFi-джиттера (типичный пик 100-200мс на ESP8266).
     *     pool_size = buffer_ms / frame_ms, clamped to [min, max].
     * Затем проверяем, что суммарная память пулов не превышает 25% free heap.
     * Если превышает - пропорционально уменьшаем оба пула. */
    int samples_per_pcm = s_samples_per_frame * s_channels;
    bool need_int32 = (s_bits_per_sample == 24) && (s_codec_mode == CODEC_MODE_PCM);
    size_t pcm_elem_size = need_int32 ? sizeof(int32_t) : sizeof(int16_t);
    int pcm_frame_bytes = (int)sizeof(pcm_frame_t) + samples_per_pcm * (int)pcm_elem_size;
    int adpcm_frame_bytes = (int)sizeof(adpcm_frame_t) + s_pkt_data_len;

    s_pcm_pool_size = 100 / s_frame_ms;
    s_adpcm_pool_size = 200 / s_frame_ms;
    if (s_pcm_pool_size < 3)
        s_pcm_pool_size = 3;
    if (s_pcm_pool_size > 8)
        s_pcm_pool_size = 8;
    if (s_adpcm_pool_size < 4)
        s_adpcm_pool_size = 4;
    if (s_adpcm_pool_size > 16)
        s_adpcm_pool_size = 16;

    /* Memory budget: max 40% of free heap for both pools combined.
     * Was 25%, but stereo@48kHz needs pcm=2/adpcm=3 (too small -> wdt reset
     * when ADPCM task blocks I2S on pool starvation). 40% gives pcm=4/adpcm=5
     * for stereo@48kHz while keeping ~38 KB free for WiFi/lwIP (enough).
     * For mono@16kHz the cap doesn't even trigger (pools are tiny). */
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t pool_mem = (uint32_t)(s_pcm_pool_size * pcm_frame_bytes +
                                   s_adpcm_pool_size * adpcm_frame_bytes);
    uint32_t mem_budget = free_heap * 2 / 5; /* 40% */
    if (pool_mem > mem_budget && pool_mem > 0)
    {
        int scale = (int)(mem_budget * 100U / pool_mem);
        s_pcm_pool_size = (s_pcm_pool_size * scale) / 100;
        s_adpcm_pool_size = (s_adpcm_pool_size * scale) / 100;
        if (s_pcm_pool_size < 2)
            s_pcm_pool_size = 2;
        if (s_adpcm_pool_size < 2)
            s_adpcm_pool_size = 2;
        ESP_LOGW(TAG, "Pool sizes reduced to fit memory: pcm=%d adpcm=%d (heap=%u)",
                 s_pcm_pool_size, s_adpcm_pool_size, (unsigned)free_heap);
    }

    ESP_LOGI(TAG, "Pools: pcm=%dx%d=%uB, adpcm=%dx%d=%uB (heap=%u, budget=%u)",
             s_pcm_pool_size, pcm_frame_bytes,
             (unsigned)(s_pcm_pool_size * pcm_frame_bytes),
             s_adpcm_pool_size, adpcm_frame_bytes,
             (unsigned)(s_adpcm_pool_size * adpcm_frame_bytes),
             (unsigned)free_heap, (unsigned)mem_budget);

    pcm_free_queue = xQueueCreate(s_pcm_pool_size, sizeof(pcm_frame_t *));
    pcm_filled_queue = xQueueCreate(s_pcm_pool_size, sizeof(pcm_frame_t *));
    adpcm_free_queue = xQueueCreate(s_adpcm_pool_size, sizeof(adpcm_frame_t *));
    adpcm_filled_queue = xQueueCreate(s_adpcm_pool_size, sizeof(adpcm_frame_t *));
    if (!pcm_free_queue || !pcm_filled_queue ||
        !adpcm_free_queue || !adpcm_filled_queue)
    {
        ESP_LOGE(TAG, "Failed to create queues");
        goto cleanup_on_fail;
    }

    for (int i = 0; i < s_pcm_pool_size; i++)
    {
        pcm_frame_t *f = pcm_frame_alloc(samples_per_pcm, s_bits_per_sample, s_codec_mode);
        if (!f)
        {
            ESP_LOGE(TAG, "PCM alloc fail");
            goto cleanup_on_fail;
        }
        xQueueSend(pcm_free_queue, &f, 0);
    }

    for (int i = 0; i < s_adpcm_pool_size; i++)
    {
        adpcm_frame_t *f = adpcm_frame_alloc(s_pkt_data_len);
        if (!f)
        {
            ESP_LOGE(TAG, "ADPCM alloc fail");
            goto cleanup_on_fail;
        }
        xQueueSend(adpcm_free_queue, &f, 0);
    }

    /* 8. Per-task done semaphores. */
    for (int i = 0; i < TASK_IDX_COUNT; i++)
    {
        s_task_done_sems[i] = xSemaphoreCreateBinary();
        if (!s_task_done_sems[i])
        {
            ESP_LOGE(TAG, "Failed to create task done semaphore %d", i);
            goto cleanup_on_fail;
        }
    }

    /* 9. Create pipeline tasks. */
    xEventGroupSetBits(s_stream_evt_grp, STREAM_EVT_ACTIVE);

    /* Choose encoder task: adpcm_task_fn for ADPCM, pcm_task_fn for PCM. */
    TaskFunction_t enc_task_fn = (s_codec_mode == CODEC_MODE_PCM)
                                     ? pcm_task_fn
                                     : adpcm_task_fn;
    const char *enc_task_name = (s_codec_mode == CODEC_MODE_PCM)
                                    ? "pcm"
                                    : "adpcm";

    if (xTaskCreate(i2s_task_fn, "i2s", TASK_STACK_I2S,
                    (void *)TASK_IDX_I2S, TASK_PRIO_I2S, &s_task_handles[TASK_IDX_I2S]) != pdPASS ||
        xTaskCreate(enc_task_fn, enc_task_name, TASK_STACK_ADPCM,
                    (void *)TASK_IDX_ADPCM, TASK_PRIO_ADPCM, &s_task_handles[TASK_IDX_ADPCM]) != pdPASS ||
        xTaskCreate(stream_task_fn, "tx", TASK_STACK_UDP,
                    (void *)TASK_IDX_UDP, TASK_PRIO_UDP, &s_task_handles[TASK_IDX_UDP]) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create pipeline tasks (out of memory?)");
        goto cleanup_on_fail;
    }

    /* Notify mode-specific subsystems that streaming has started. */
    ops->on_stream_started();

    ESP_LOGI(TAG, "Streaming started - %u Hz, %d ms frames, %u ch",
             (unsigned)s_sample_rate, s_frame_ms, s_channels);
    return ESP_OK;

cleanup_on_fail:
    xEventGroupClearBits(s_stream_evt_grp, STREAM_EVT_ACTIVE);

    /* CRITICAL FIX (C1): Close transport FIRST so any task blocked in
     * sendto() returns immediately (EBADF) and can exit cleanly. */
    transport_deinit();

    /* FIX (C3): Wait for any already-created pipeline tasks to exit before
     * deleting semaphores/queues. Without this, a task created at step 9
     * (before the failure) could still be accessing them -> use-after-free. */
    for (int i = 0; i < TASK_IDX_COUNT; i++)
    {
        if (s_task_handles[i])
        {
            wait_for_task_exit(i, STREAM_STOP_TIMEOUT_MS);
        }
    }

    /* FIX (C4): NULL the semaphore pointer BEFORE deleting the semaphore.
     * Higher-priority pipeline tasks could read the non-NULL pointer and
     * call xSemaphoreGive on a freed semaphore -> use-after-free. */
    for (int i = 0; i < TASK_IDX_COUNT; i++)
    {
        SemaphoreHandle_t tmp = s_task_done_sems[i];
        s_task_done_sems[i] = NULL;
        if (tmp)
        {
            vSemaphoreDelete(tmp);
        }
    }

    drain_and_delete_queue(&pcm_free_queue);
    drain_and_delete_queue(&pcm_filled_queue);
    drain_and_delete_queue(&adpcm_free_queue);
    drain_and_delete_queue(&adpcm_filled_queue);

    for (int i = 0; i < 2; i++)
    {
        if (adpcm_enc[i])
        {
            adpcm_enc_destroy(adpcm_enc[i]);
            adpcm_enc[i] = NULL;
        }
    }
    i2s_capture_deinit();
    /* Same 50ms delay as in stop_streaming() - I2S hardware needs time to
     * power down before a potential restart. See comment in stop_streaming. */
    vTaskDelay(pdMS_TO_TICKS(50));

    stream_mode_ops()->on_stream_stopped();

    return ESP_FAIL;
}

static void stop_streaming(void)
{
    if (!streaming_is_active())
    {
        ESP_LOGW(TAG, "Not streaming");
        return;
    }

    ESP_LOGI(TAG, "Stopping streaming...");

    /* Signal pipeline tasks to exit. The I2S task uses a short i2s_read
     * timeout so it re-checks this flag within ~50ms. */
    xEventGroupClearBits(s_stream_evt_grp, STREAM_EVT_ACTIVE);

    /* CRITICAL FIX (C1): Close the UDP socket / raw TX transport BEFORE
     * waiting for pipeline tasks to exit.
     *
     * Why: stream_task_fn may be blocked in sendto() with SO_SNDTIMEO=2000ms.
     * If we wait_for_task_exit first (timeout 500ms), the task gets force-
     * deleted while inside lwIP sendto() - corrupting lwIP state and orphaning
     * any mutex it held. Closing the socket first makes sendto() return EBADF
     * immediately, so the task exits cleanly via its normal path.
     *
     * This was the root cause of the HOTRESTART crash: force-deleted task
     * orphaned svc_port::s_mutex -> next start_streaming() deadlocked in
     * svc_port_clear_error() -> watchdog reboot after ~8s. */
    /* Close transport (TCP: only client conn, keep listener alive for restart;
     * UDP/RawTX: full deinit). This unblocks any task stuck in send()/sendto()
     * so it exits cleanly — see the EBADF comment above. */
    transport_close_client();

    /* Wait for each task to exit cleanly. */
    int clean_exits = 0;
    for (int i = 0; i < TASK_IDX_COUNT; i++)
    {
        if (wait_for_task_exit(i, STREAM_STOP_TIMEOUT_MS))
        {
            clean_exits++;
        }
    }

    /* Clean up semaphores.
     * FIX (C4): NULL the pointer BEFORE deleting - pipeline tasks may still
     * be reading it during their exit path (higher priority than main). */
    for (int i = 0; i < TASK_IDX_COUNT; i++)
    {
        SemaphoreHandle_t tmp = s_task_done_sems[i];
        s_task_done_sems[i] = NULL;
        if (tmp)
        {
            vSemaphoreDelete(tmp);
        }
    }

    /* Drain queues. */
    drain_and_delete_queue(&pcm_free_queue);
    drain_and_delete_queue(&pcm_filled_queue);
    drain_and_delete_queue(&adpcm_free_queue);
    drain_and_delete_queue(&adpcm_filled_queue);

    /* Destroy encoders. */
    for (int i = 0; i < 2; i++)
    {
        if (adpcm_enc[i])
        {
            adpcm_enc_destroy(adpcm_enc[i]);
            adpcm_enc[i] = NULL;
        }
    }

    i2s_capture_deinit();
    /* Give I2S hardware time to fully power down before a potential restart.
     * Without this delay, a rapid CONFIGURE (stop+start) or AT+HOTRESTART
     * causes LoadStoreAlignment crash during DMA queue rebuild in the next
     * i2s_driver_install(). The ESP8266 I2S peripheral needs time after
     * i2s_driver_uninstall() before its internal state is safe to re-init. */
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Transport was already closed above (transport_close_client, before
     * wait_for_task_exit) - see C1 fix comment. */

    stream_mode_ops()->on_stream_stopped();

    ESP_LOGI(TAG, "Streaming stopped (%d/3 tasks exited cleanly)", clean_exits);
}

/* ====================================================================
 * app_main
 * ==================================================================== */

void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP8266 WiFi Microphone " FIRMWARE_VERSION " (DVI4/RFC 3551) ===");
    ESP_LOGI(TAG, "Audio: %u Hz, %u ch, %u bps (frame_ms computed at stream start)",
             (unsigned)AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, (unsigned)AUDIO_BITRATE);
    ESP_LOGI(TAG, "AT command interface on UART0 (%d 8N1)", UART_BAUD_RATE);

    /* 0. NVS (must be first - battery and config depend on it). */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "NVS partition needs erase - erasing...");
        nvs_flash_erase();
        err = nvs_flash_init();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "NVS init failed after erase: %s", esp_err_to_name(err));
            return;
        }
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return;
    }

    /* 1. Battery monitoring (optional, ported from ESP8285-WEBSERVER).
     * Initializes ADC and starts a background task that checks V_batt
     * every BATT_CHECK_MIN minutes. If V_batt < BATT_CRITICAL_MV, device
     * enters deep sleep to preserve battery.
     * Fully excluded from build when BATTERY_ENABLED=0 in menuconfig. */
#if BATTERY_ENABLED
    err = battery_init();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Battery init failed (continuing anyway)");
    }
    if (xTaskCreate(battery_monitor_task, "bat", BATT_TASK_STACK,
                    NULL, BATT_TASK_PRIO, NULL) != pdPASS)
    {
        ESP_LOGW(TAG, "Failed to create battery_monitor_task (continuing)");
    }
#else
    ESP_LOGI(TAG, "Battery monitoring disabled (menuconfig)");
#endif

    /* 2. Config manager (requires NVS). */
    err = config_mgr_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Config manager init failed");
        return;
    }

    /* 3. Stream control EventGroup. */
    s_stream_evt_grp = xEventGroupCreate();
    if (!s_stream_evt_grp)
    {
        ESP_LOGE(TAG, "Failed to create stream event group");
        return;
    }

    /* 5. WiFi init - mode-specific (UDP: connect to AP; RAWTX: radio+channel). */
    device_config_t cfg;
    config_get_copy(&cfg);

    /* Select stream mode (UDP or Raw TX) based on config.
     * Must be called before any ops->xxx() usage. */
    stream_mode_init(&cfg);

    err = stream_mode_ops()->wifi_init(&cfg);
    if (err == ESP_OK)
    {
        s_wifi_initialized = true;
        /* Block until WiFi is ready (UDP: wait for AP association;
         * RAWTX: no-op). Ignoring errors - start_streaming will retry. */
        stream_mode_ops()->wifi_wait_ready(&cfg);
    }
    else
    {
        ESP_LOGW(TAG, "WiFi init failed, will retry at stream start");
    }

    /* 6. Service port (UDP/TCP: starts EASSP listener; RAWTX: no-op). */
    err = stream_mode_ops()->svc_port_init(s_stream_evt_grp, cfg.svc_port);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Service port init failed: %s", esp_err_to_name(err));
    }
    else if (stream_mode_ops()->uses_svc_port)
    {
        ESP_LOGI(TAG, "Service port started on UDP:%u - waiting for server...",
                 (unsigned)cfg.svc_port);
    }

    /* Initialize s_channels from NVS config.
     * For UDP mode, svc_port must be initialized first - the ops table
     * handles this correctly (udp_set_channels calls svc_port_set_channels,
     * rawtx_set_channels is a no-op). */
    s_channels = i2s_capture_channel_count(cfg.channel_format);
    stream_mode_ops()->set_channels(s_channels);
    ESP_LOGI(TAG, "Config from NVS: ch=%u (fmt=%u)", s_channels, cfg.channel_format);

    /* 7. AT command interface. */
#if AT_CMD_ENABLED
    at_cmd_init();
#endif

    /* 8. Auto-start streaming if the mode requires it
     *    (Raw TX: yes - no server to send CONFIGURE; UDP: no - waits
     *    for server to discover and configure us). */
    if (stream_mode_ops()->auto_start)
    {
        ESP_LOGI(TAG, "Auto-starting stream (%s mode)...",
                 stream_mode_ops()->name);
        xEventGroupSetBits(s_stream_evt_grp, STREAM_EVT_START_REQ);
    }

    /* 9. Main loop - wait for START_REQ / STOP_REQ from svc_port. */
    ESP_LOGI(TAG, "Main loop running");
    while (1)
    {
        EventBits_t bits = xEventGroupWaitBits(s_stream_evt_grp,
                                               STREAM_EVT_START_REQ | STREAM_EVT_STOP_REQ,
                                               pdTRUE, pdFALSE,
                                               portMAX_DELAY);

        if (bits & STREAM_EVT_STOP_REQ)
        {
            stop_streaming();
            /* Critical: yield between stop and start. When HOTRESTART sets
             * both STOP_REQ + START_REQ, this delay gives exited pipeline
             * tasks time to fully die (vTaskDelete is async), WiFi/lwIP to
             * settle, and heap to stabilize. Without it, start_streaming()
             * runs 2ms after stop_streaming() returns, causing heap/UART
             * corruption (confirmed by err4.txt - crash at "UDP: WiFi co").
             * 200ms chosen empirically: 50ms (i2s_capture_deinit delay) is
             * not enough; 200ms allows full task cleanup. */
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        if (bits & STREAM_EVT_START_REQ)
        {
            start_streaming();
        }
    }
}
 