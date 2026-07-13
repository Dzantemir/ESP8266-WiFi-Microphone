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
#include "esp_sleep.h"  /* FIX (wifi-boot-retry): esp_deep_sleep for WiFi boot retry */
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

/* FIX (GROK-22): the PCM frame's flexible array is declared as int16_t[]
 * but in PCM 24-bit mode the buffer is allocated, written (memcpy of
 * int32_t) and read (int32_t* cast) as int32_t[]. Under C11 strict
 * aliasing (and -fstrict-aliasing at -O2) this is UB. The clean fix is
 * to declare the flexible array as uint8_t[] — the "char array" aliasing
 * rule (C11 6.5p7) explicitly allows any object type to be accessed
 * through a pointer to char/uint8_t, and conversely a uint8_t[] buffer
 * may be cast to any type the caller knows it actually holds.
 *
 * Callers that previously wrote `pcm->samples` now write
 * `pcm->samples_raw` and cast to the appropriate type. The helper
 * inlines pcm_samples16() / pcm_samples32() make the casts explicit
 * and self-documenting. */
typedef struct
{
    int num_samples;
    uint8_t samples_raw[];
} pcm_frame_t;

static inline int16_t *pcm_samples16(pcm_frame_t *f) { return (int16_t *)f->samples_raw; }
static inline int32_t *pcm_samples32(pcm_frame_t *f) { return (int32_t *)f->samples_raw; }
static inline const int16_t *pcm_samples16_const(const pcm_frame_t *f) { return (const int16_t *)f->samples_raw; }
static inline const int32_t *pcm_samples32_const(const pcm_frame_t *f) { return (const int32_t *)f->samples_raw; }

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

#ifdef CONFIG_STREAMER_SUPERVISOR_ENABLED
/* ---- Supervisor liveness counters ----
 * Incremented by I2S and TX tasks on each successful frame/send.
 * Read by supervisor_task_fn to detect pipeline deadlocks. */
static volatile uint32_t s_supervisor_i2s_count = 0;
static volatile uint32_t s_supervisor_tx_count = 0;
static volatile TickType_t s_supervisor_stream_start_tick = 0;
#endif /* CONFIG_STREAMER_SUPERVISOR_ENABLED */

/* ---- Stream control EventGroup ---- */

static EventGroupHandle_t s_stream_evt_grp = NULL;
static bool s_wifi_initialized = false;

/* FIX (AUDIT-XPORT-AUTOAPPLY): pending transport change flag.
 *
 * User's desired behavior:
 *   1. Stream running on UDP. User does AT+XPORT=1 (TCP).
 *      -> Save to NVS, but stream CONTINUES on UDP. No immediate switch.
 *   2. User does AT+HOTRESTART -> apply the pending TCP transport (on the fly
 *      for UDP<->TCP, refused with warning for RAWTX).
 *      OR user does AT+RST (reboot) -> apply on next boot.
 *   3. Server-initiated stop+start (CMD_STOP + CONFIGURE) does NOT apply the
 *      pending transport change. The stream restarts with the OLD transport.
 *      This prevents the server from triggering an unwanted transport switch
 *      when it does its routine stop+start (e.g. after a TCP send error).
 *
 * Implementation: s_pending_transport_apply is set to true ONLY by
 * streaming_request_restart() (which AT+HOTRESTART calls). start_streaming()
 * checks it: if a transport change is detected in NVS but the flag is false,
 * the change is REFUSED (old transport stays). After applying or refusing,
 * the flag is cleared. On AT+RST (reboot), the flag is naturally reset (BSS)
 * and the new transport is loaded from NVS at boot. */
static bool s_pending_transport_apply = false;

/* Pool sizes - computed at start_streaming from free heap & frame_ms. */
static int s_pcm_pool_size = 4;
static int s_adpcm_pool_size = 6;

/* Frame duration (ms) - computed in start_streaming from I2S params. */
static uint32_t s_frame_ms = 20;
/* FIX (GROK-21): tracks whether s_frame_ms has ever been computed by a
 * successful start_streaming() call. Before the first start, the static
 * initializer gives s_frame_ms = 20, which svc_port INFO / AT+GMR /
 * AT+STATUS would report as if it were the runtime value — misleading
 * (the actual runtime value is computed from rate/channels/codec and
 * ranges 5..60 ms). Callers can now distinguish "not yet started" from
 * "real value" via streaming_frame_ms_known(). */
static bool s_frame_ms_known = false;

/* Accessor for other modules (svc_port, at_cmd) - returns current frame_ms. */
uint32_t streaming_get_frame_ms(void)
{
    /* FIX (L25): return the actual computed frame_ms (s_frame_ms) which is
     * updated in start_streaming(). The default of 20 is only seen before
     * the first stream start - documented in AT+GMR / AT+STATUS as
     * "post-last-stream value".
     *
     * FIX (GROK-21): callers that want to distinguish "no stream ever
     * started" from "real value" should call streaming_frame_ms_known()
     * first. Legacy callers that just want a number (e.g. packet header
     * bitrate computation) still get 20 as a placeholder, which is what
     * they always got — no behavior change for them. */
    return s_frame_ms;
}

bool streaming_frame_ms_known(void)
{
    return s_frame_ms_known;
}

/* FIX (B3/channels-desync): dual-path channel count for INFO/STATUS.
 *
 *   - Stream ACTIVE: return main::s_channels (the channel count the
 *     running stream is ACTUALLY using). Prevents B3 desync where
 *     AT+CH=2 updates NVS but the stream is still 1 ch — INFO must
 *     report 1 so the receiver doesn't mis-allocate stereo buffers.
 *
 *   - Stream NOT active (IDLE / boot / between stop and start): return
 *     the NVS config channel count (what the NEXT stream will use).
 *     This fixes the user's bug: after AT+CH=2 in IDLE, INFO now shows
 *     "stereo" immediately, without waiting for start_streaming().
 *
 * CRITICAL: config_get_copy() takes the config_mgr mutex (NOT svc_port's
 * s_mutex), so calling this from build_info_payload (which holds s_mutex)
 * does NOT deadlock. However, for lock-ordering hygiene, build_info_payload
 * calls this BEFORE taking s_mutex.
 *
 * Race safety: streaming_is_active() reads the ACTIVE bit atomically.
 * In start_streaming(), ACTIVE is set AFTER s_channels is updated, so a
 * concurrent read either sees ACTIVE=0 (returns NVS, which equals the
 * about-to-be-set s_channels) or ACTIVE=1 (returns new s_channels). */
uint8_t streaming_get_channels(void)
{
    if (streaming_is_active())
        return s_channels;
    /* IDLE: return the config (pending) channel count from NVS. */
    device_config_t cfg;
    config_get_copy(&cfg);
    return channel_format_to_count(cfg.channel_format);
}

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
    /* FIX (AUDIT-XPORT-AUTOAPPLY): mark that this restart is user-initiated
     * (via AT+HOTRESTART). start_streaming() will check this flag to decide
     * whether to apply a pending transport_mode change from NVS. Only an
     * explicit user HOTRESTART may apply it - server-initiated stop+start
     * must NOT change the transport. */
    s_pending_transport_apply = true;
    /* FIX (H5): set BOTH bits in a single xEventGroupSetBits call. The
     * previous two-call sequence had a check-then-set race: between
     * is_active() and the bit sets, the main loop could process a prior
     * STOP_REQ and clear ACTIVE. The subsequent START_REQ then auto-started
     * a stream the user/server intended to keep stopped. Setting both bits
     * atomically lets the main loop re-check is_active() before honoring
     * START, and processes them in the order STOP then START. */
    xEventGroupSetBits(s_stream_evt_grp,
                       STREAM_EVT_STOP_REQ | STREAM_EVT_START_REQ);
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
    int idx = (int)(intptr_t)arg;  /* FIX (L26): intptr_t cast */
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
    /* FIX (GROK-2.3): partial-read underrun counter. When i2s_capture_read
     * returns ESP_OK with n < total, the missing samples are zero-padded
     * and the frame is sent as if complete — masking the underrun as
     * silence. Track consecutive partial reads so we can log + signal
     * SVC_ERR_I2S after N in a row, instead of silently degrading audio. */
    uint32_t partial_count = 0;

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
#ifdef CONFIG_STREAMER_SUPERVISOR_ENABLED
        s_supervisor_i2s_count++;
#endif
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

        /* FIX (GROK-2.3): detect partial reads (n < total). The zero-pad
         * below masks the underrun as silence — without this counter the
         * server receives a full-length frame with embedded silence but
         * has no way to know. Track consecutive partials; after 5 in a
         * row, log + signal SVC_ERR_I2S so the receiver can take action
         * (e.g. PLC, jitter buffer refill). Reset on a full read. */
        if (n < total)
        {
            partial_count++;
            if (partial_count == 1 || (partial_count % 50) == 0)
            {
                ESP_LOGW(TAG, "[I2S] partial read #%u: got %d/%d samples - "
                              "zero-padding (underrun masked as silence)",
                         (unsigned)partial_count, n, total);
            }
            if (partial_count == 5)
            {
                ESP_LOGE(TAG, "[I2S] %u consecutive partial reads - signaling "
                              "SVC_ERR_I2S", (unsigned)partial_count);
                svc_port_set_error(SVC_ERR_I2S);
            }
        }
        else
        {
            /* FIX (B4 REGRESSION): GROK-2.3 added svc_port_set_error(SVC_ERR_I2S)
             * after 5 partial reads, but did NOT clear it on recovery. This
             * left INFO/status stuck in ERROR/I2S forever after a brief underrun.
             * Now: if we had signaled an error (partial_count >= 5) and I2S has
             * recovered (full read), clear the error so INFO/status reflect
             * the actual healthy state. */
            if (partial_count >= 5)
            {
                ESP_LOGI(TAG, "[I2S] recovered from partial reads - clearing SVC_ERR_I2S");
                svc_port_clear_error();
            }
            partial_count = 0;
        }

        for (int i = n; i < total; i++)
            raw[i] = 0;

        if (s_codec_mode == CODEC_MODE_PCM && is_24bit)
        {
            /* PCM 24-bit: copy raw int32 samples directly (sign-extended
             * 24-bit values). pcm_task_fn will strip the high byte and
             * emit 3 bytes per sample. No dither - we want full 24-bit
             * precision in the stream. */
            memcpy(pcm_samples32(pcm), raw, (size_t)total * sizeof(int32_t));
        }
        else if (is_24bit)
            dither_buffer_24_to_16(raw, pcm_samples16(pcm), total);
        else
            dither_buffer_passthrough(raw, pcm_samples16(pcm), total);

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
    int idx = (int)(intptr_t)arg;  /* FIX (L26): intptr_t cast */
    ESP_LOGI(TAG, "[ADPCM] Task started (idx=%d, %d ch)", idx, s_channels);

    uint32_t frame_count = 0;
    /* FIX (FW#1): removed seq_counter from encoder task. seq is now
     * assigned in the TX task AFTER successful transport_send, so
     * dropped frames don't create phantom seq gaps that the server
     * would misinterpret as packet loss (triggering PLC clicks). */
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
            err = adpcm_enc_process(adpcm_enc[0], pcm_samples16(pcm), pcm->num_samples,
                                    adpcm->data, cap, &written);
        }
        else
        {
            /* Stereo: deinterleave L,R,L,R -> ch_left[], ch_right[] */
            int16_t *samples = pcm_samples16(pcm);
            for (int i = 0; i < s_samples_per_frame; i++)
            {
                ch_left[i] = samples[i * 2];
                ch_right[i] = samples[i * 2 + 1];
            }
            /* FIX (GROK-14): pass the REMAINING buffer size to the second
             * adpcm_enc_process call, not the per-channel `cap`. Today the
             * invariant s_pkt_data_len == 2*cap holds, so this is equivalent,
             * but if a future change breaks that invariant (e.g. a different
             * per-channel payload shape, or a header size change), the
             * second call could write past s_pkt_data_len. Defensive coding:
             * track `rem` and subtract each channel's written bytes. */
            size_t rem = (size_t)s_pkt_data_len;
            size_t wl = 0, wr = 0;
            err = adpcm_enc_process(adpcm_enc[0], ch_left, s_samples_per_frame,
                                    adpcm->data, rem, &wl);
            rem -= wl;
            if (err == ESP_OK)
                err = adpcm_enc_process(adpcm_enc[1], ch_right, s_samples_per_frame,
                                        adpcm->data + wl, rem, &wr);
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
        /* FIX (FW#1): seq_num assigned in TX task after successful send. */
        adpcm->seq_num = 0;
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
    int idx = (int)(intptr_t)arg;  /* FIX (L26): intptr_t cast */
    ESP_LOGI(TAG, "[PCM] Task started (idx=%d, %d ch, %d-bit)",
             idx, s_channels, s_bits_per_sample);

    uint32_t frame_count = 0;
    /* FIX (FW#1): removed seq_counter - assigned in TX task. */

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
            /* 16-bit: samples already int16 in pcm->samples_raw[]. Just copy. */
            size_t bytes = (size_t)n * sizeof(int16_t);
            if (bytes > (size_t)s_pkt_data_len)
                bytes = s_pkt_data_len;
            memcpy(dst, pcm_samples16(pcm), bytes);
            written = bytes;
        }
        else
        {
            /* 24-bit: pcm->samples_raw[] actually holds int32_t (sign-extended
             * 24-bit values, copied raw from i2s_capture_read). Emit only
             * the low 3 bytes (LE) per sample, stripping the redundant
             * high byte. */
            int32_t *s32 = pcm_samples32(pcm);
            for (int i = 0; i < n; i++)
            {
                /* FIX (H4): bounds check BEFORE the 3-byte write. The
                 * previous code wrote 3 bytes first then checked, so if
                 * s_pkt_data_len % 3 != 0 (or < 3 from a future bug) the
                 * first iteration wrote past the packet buffer -> heap
                 * corruption / overwritten pkt_header_t. */
                if (written + 3 > (size_t)s_pkt_data_len)
                    break;
                int32_t s = s32[i];
                dst[written++] = (uint8_t)(s & 0xFF);
                dst[written++] = (uint8_t)((s >> 8) & 0xFF);
                dst[written++] = (uint8_t)((s >> 16) & 0xFF);
            }
        }

        out->data_len = (uint16_t)written;
        /* FIX (FW#1): seq_num assigned in TX task after successful send. */
        out->seq_num = 0;
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
    int idx = (int)(intptr_t)arg;  /* FIX (L26): intptr_t cast */
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
    /* FIX (FW#1): seq_counter lives in the TX task, not the encoder.
     * It's incremented ONLY after a successful transport_send, so
     * dropped frames (transport not ready / send fail) don't create
     * phantom seq gaps. The server sees a contiguous seq stream and
     * doesn't fire PLC for frames that were never actually sent. */
    uint16_t seq_counter = 0;

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
        /* FIX (FW#1): assign seq HERE (in TX task), not in encoder. The
         * seq is only committed (seq_counter++) after successful send below. */
        adpcm->seq_num = seq_counter;
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
#ifdef CONFIG_STREAMER_SUPERVISOR_ENABLED
            s_supervisor_tx_count++;
#endif
            /* FIX (FW#1): commit seq only on successful send. If send fails,
             * seq_counter stays the same -> next frame reuses this seq ->
             * server sees no gap -> no false PLC -> no click. */
            seq_counter++;
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
        /* FIX (GROK-18): the previous log only said "force deleting"
         * without explaining the consequence. Force-deleting a task
         * blocked inside lwIP send()/sendto() orphans any mutex the
         * task held (e.g. svc_port::s_mutex), which can deadlock the
         * next start_streaming() and trigger a watchdog reboot ~8 s
         * later. Make the consequence explicit so the operator knows
         * to issue AT+RST proactively rather than waiting for the WDT. */
        ESP_LOGE(TAG, "Task %d did not exit in %u ms - force deleting. "
                 "WARNING: this may orphan lwIP/svc_port mutexes and "
                 "deadlock the next stream start. REBOOT RECOMMENDED (AT+RST).",
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

    /* FIX (A1 defense-in-depth): refuse to start if any pipeline task is
     * still alive. The main loop normally serializes STOP→START, but this
     * guard catches any future code path that might call start_streaming()
     * directly (bypassing the event loop). Without this, a start during an
     * incomplete stop would create duplicate tasks racing on the same I2S
     * DMA / queues / transport. */
    for (int i = 0; i < TASK_IDX_COUNT; i++)
    {
        if (s_task_handles[i] != NULL)
        {
            ESP_LOGE(TAG, "start_streaming: task %d handle non-NULL - stop not complete", i);
            return ESP_ERR_INVALID_STATE;
        }
    }

    device_config_t cfg;
    config_get_copy(&cfg);

    /* Re-select transport mode from config. This is CRITICAL for AT+HOTRESTART:
     * AT+XPORT changes cfg.transport_mode in NVS, but s_active_ops was set
     * once at boot. Without this call, switching UDP<->TCP via AT+XPORT +
     * HOTRESTART has no effect — the old transport stays active.
     *
     * FIX (AUDIT-XPORT-AUTOAPPLY): AT+XPORT saves the new transport to NVS
     * but does NOT apply it immediately. The stream continues with the OLD
     * transport. The transport change is applied ONLY when:
     *   - AT+HOTRESTART is explicitly requested by the user (sets
     *     s_pending_transport_apply = true), OR
     *   - AT+RST reboots the device (NVS is loaded fresh at boot).
     * Server-initiated stop+start (CMD_STOP + CONFIGURE) does NOT apply the
     * pending transport change - the stream restarts with the OLD transport.
     * This prevents the server from triggering an unwanted transport switch
     * when it does its routine stop+start (e.g. after a TCP send error).
     *
     * FIX (AUDIT-XPORT-CRASH): RAWTX(2) transitions require AT+RST.
     * - UDP(0) <-> TCP(1) is supported on the fly (via HOTRESTART only):
     *   both share the same AP-associated WiFi path.
     * - Any transition involving RAWTX(2) CANNOT be applied on the fly
     *   (WiFi driver reinit crashes on ESP8266 RTOS SDK v3.4). We KEEP the
     *   old transport and continue. AT+HOTRESTART still applies all other
     *   settings (audio, WiFi ch). The user must do AT+RST to switch
     *   to/from RAWTX.
     *
     * IMPORTANT: stream_mode_init() immediately overwrites s_active_ops and
     * s_active_transport. So we capture the OLD transport BEFORE calling it. */
    uint8_t old_transport = stream_mode_current_transport();
    const stream_mode_ops_t *old_ops = stream_mode_ops();
    const stream_mode_ops_t *ops = old_ops;   /* default: no change */
    bool transport_changed_in_nvs = (cfg.transport_mode != old_transport);

    if (transport_changed_in_nvs && !s_pending_transport_apply)
    {
        /* Server-initiated stop+start (NOT a user HOTRESTART). Refuse the
         * transport change - keep the old transport. The user must explicitly
         * do AT+HOTRESTART or AT+RST to apply the change. Do NOT call
         * stream_mode_init() with the new cfg - it would overwrite
         * s_active_ops. Just log and continue with old_ops. */
        ESP_LOGW(TAG, "Transport changed in NVS (%s -> %s) but no "
                      "HOTRESTART requested - keeping old transport (%s). "
                      "Use AT+HOTRESTART or AT+RST to apply.",
                 old_ops->name,
                 (cfg.transport_mode == TRANSPORT_MODE_UDP) ? "UDP" :
                 (cfg.transport_mode == TRANSPORT_MODE_TCP) ? "TCP" :
                 "Raw 802.11 TX",
                 old_ops->name);
        /* Override cfg.transport_mode to the OLD value so the rest of
         * start_streaming (logging, packet header) uses the old transport.
         * s_active_ops stays as old_ops (we never called stream_mode_init). */
        cfg.transport_mode = old_transport;
        /* ops == old_ops (no change). Stream starts with OLD transport. */
    }
    else if (transport_changed_in_nvs)
    {
        /* HOTRESTART was explicitly requested. Apply the transport change. */
        stream_mode_init(&cfg);
        ops = stream_mode_ops();

        if (old_ops->needs_wifi_association != ops->needs_wifi_association)
        {
            /* FIX (AUDIT-XPORT-CRASH): RAWTX transitions cannot be applied on
             * the fly (WiFi driver reinit crashes on ESP8266 RTOS SDK v3.4).
             * Don't fail the whole start_streaming - revert to the old
             * transport so AT+HOTRESTART still applies audio settings. The
             * user must do AT+RST to switch to/from RAWTX. */
            ESP_LOGW(TAG, "Transport change %s -> %s requires AT+RST "
                          "(WiFi mode differs). Keeping old transport (%s) "
                          "for this stream. Other settings (audio, WiFi ch) "
                          "are still applied.",
                     old_ops->name, ops->name, old_ops->name);
            /* Revert stream_mode to the old transport. */
            cfg.transport_mode = old_transport;
            stream_mode_init(&cfg);
            ops = stream_mode_ops();   /* ops == old_ops again */
        }
        else
        {
            /* UDP <-> TCP transition via HOTRESTART - safe to apply on the fly. */
            ESP_LOGI(TAG, "Transport changed -- deinitializing old transport (%s)",
                     old_ops->name);
            old_ops->deinit();
            if (old_ops->uses_svc_port && !ops->uses_svc_port)
            {
                ESP_LOGI(TAG, "Switching to non-svc_port transport - deinit svc_port");
                svc_port_deinit();
            }
        }
    }
    /* Clear the pending flag - the change has been applied, refused, or
     * there was no change. Either way, the next start_streaming requires
     * a fresh HOTRESTART to apply a transport change. */
    s_pending_transport_apply = false;

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
        s_samples_per_frame &= ~7;   /* кратно 8: 661->656, 882->880, 220->216 */
    else
        s_samples_per_frame &= ~3;   /* кратно 4 (24-bit) */

    /* FIX (FW#2): recompute s_frame_ms from the ALIGNED samples_per_frame.
     * Previously s_frame_ms was computed from the UNALIGNED value, so the
     * frame_ms field in the packet header didn't match the actual frame
     * duration (samples_per_frame / rate). The server used this frame_ms
     * for skip frame count (STARTUP_SKIP_MS / frame_ms) and jitter timing.
     * A mismatch caused the skip to end at the wrong time -> click at
     * startup. Also, timestamp_ms = frame_count * s_frame_ms drifted from
     * real wallclock time, affecting server HOTRESTART detection. */
    s_frame_ms = (uint32_t)((uint64_t)s_samples_per_frame * 1000 / s_sample_rate);
    /* FIX (GROK-21): mark s_frame_ms as valid so streaming_get_frame_ms()
     * callers (and the new streaming_frame_ms_known() predicate) can
     * distinguish "real computed value" from the static-init placeholder
     * of 20 ms seen before the first successful start_streaming(). */
    s_frame_ms_known = true;

    /* FIX (M14): post-condition check. If frame_ms was so small that
     * samples_per_frame < 8, the alignment could zero it (8 & ~7 = 0),
     * leading to division by zero in pool sizing (100 / s_frame_ms) and
     * empty packets. */
    if (s_samples_per_frame < 8)
    {
        ESP_LOGE(TAG, "samples_per_frame=%d too small (frame_ms=%u, rate=%u) - aborting",
                 s_samples_per_frame, (unsigned)s_frame_ms, (unsigned)s_sample_rate);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Computed frame_ms=%u (aligned) -> samples_per_frame=%d (rate=%u, ch=%u)",
             (unsigned)s_frame_ms, s_samples_per_frame,
             (unsigned)s_sample_rate, s_channels);

    s_adpcm_frame_bytes = s_samples_per_frame / 2;
    /* Update codec mode + bit depth from config BEFORE computing bitrate.
     * Previously these were assigned AFTER the bitrate calc, so on the first
     * stream after boot (or after AT+CODEC / AT+BITS change + HOTRESTART) the
     * bitrate used stale s_codec_mode/s_bits_per_sample values — e.g. an ADPCM
     * bitrate (4 bits/sample) was reported in the packet header for a PCM
     * stream. The receiver then displays the wrong kbps and may mis-size
     * buffers. */
    s_codec_mode = cfg.codec_mode;
    s_bits_per_sample = cfg.bits_per_sample;
    s_sample_rate_enum = sample_rate_to_enum(s_sample_rate);
    /* Bitrate depends on codec: ADPCM = 4 bits/sample, PCM = bits_per_sample
     * bits/sample. Now uses the just-updated s_codec_mode/s_bits_per_sample. */
    {
        int bits_per_codec = (s_codec_mode == CODEC_MODE_PCM)
                                 ? s_bits_per_sample : 4;
        s_audio_bitrate = s_sample_rate * (uint32_t)bits_per_codec * s_channels;
    }

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
            { esp_err_t derr = i2s_capture_deinit(); if (derr != ESP_OK) ESP_LOGW(TAG, "i2s_capture_deinit: %s", esp_err_to_name(derr)); }
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
        { esp_err_t derr = i2s_capture_deinit(); if (derr != ESP_OK) ESP_LOGW(TAG, "i2s_capture_deinit: %s", esp_err_to_name(derr)); }
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
#ifdef CONFIG_STREAMER_SUPERVISOR_ENABLED
    s_supervisor_stream_start_tick = xTaskGetTickCount();
#endif

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
     * (before the failure) could still be accessing them -> use-after-free.
     * FIX (split): use the transport-specific stop timeout (UDP vs TCP)
     * so the wait always covers the corresponding send timeout. */
    uint32_t stop_to = (stream_mode_current_transport() == TRANSPORT_MODE_TCP)
                       ? STREAM_STOP_TIMEOUT_TCP_MS
                       : STREAM_STOP_TIMEOUT_UDP_MS;
    for (int i = 0; i < TASK_IDX_COUNT; i++)
    {
        if (s_task_handles[i])
        {
            wait_for_task_exit(i, stop_to);
        }
        /* FIX (B10): ensure s_task_handles[i] is NULLed even if
         * wait_for_task_exit returned false (e.g. s_task_done_sems was
         * NULL, or the task was force-deleted). Without this, the
         * supervisor's stack-high-water check (uxTaskGetStackHighWaterMark
         * on a deleted handle) is undefined behavior. */
        s_task_handles[i] = NULL;
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
    { esp_err_t derr = i2s_capture_deinit(); if (derr != ESP_OK) ESP_LOGW(TAG, "i2s_capture_deinit: %s", esp_err_to_name(derr)); }
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

    /* Wait for each task to exit cleanly.
     * FIX (split): use the transport-specific stop timeout (UDP vs TCP). */
    uint32_t stop_to = (stream_mode_current_transport() == TRANSPORT_MODE_TCP)
                       ? STREAM_STOP_TIMEOUT_TCP_MS
                       : STREAM_STOP_TIMEOUT_UDP_MS;
    int clean_exits = 0;
    for (int i = 0; i < TASK_IDX_COUNT; i++)
    {
        if (wait_for_task_exit(i, stop_to))
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

    { esp_err_t derr = i2s_capture_deinit(); if (derr != ESP_OK) ESP_LOGW(TAG, "i2s_capture_deinit: %s", esp_err_to_name(derr)); }
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

#ifdef CONFIG_STREAMER_SUPERVISOR_ENABLED
/* ====================================================================
 * Supervisor task — software watchdog
 * ====================================================================
 *
 * Runs independently of the streaming pipeline. Checks every
 * SUPERVISOR_CHECK_INTERVAL_MS:
 *   1. Free heap >= SUPERVISOR_MIN_HEAP_BYTES
 *   2. If streaming active for > SUPERVISOR_STALL_TIMEOUT_MS:
 *      I2S or TX counter must have advanced since last check
 *   3. All pipeline task stacks have > SUPERVISOR_MIN_STACK_BYTES free
 *
 * If any check fails, log the reason and call esp_restart().
 *
 * WHY: the ESP8266 hardware WDT is fed by the IDLE task, so it only
 * fires when a task hogs the CPU without yielding. The deadlocks we've
 * seen (sendto/send stuck with timeout, deadlocked mutex, heap
 * exhaustion) all involve tasks that DO yield — IDLE runs, HW WDT gets
 * fed, but no useful work happens. The supervisor catches these by
 * checking application-level liveness (frame counters) and heap health.
 *
 * The supervisor has LOW priority (TASK_PRIO_SUPERVISOR=1), so it only
 * runs when the system is idle — which is exactly when deadlocked tasks
 * are yielding. */
static void supervisor_task_fn(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Supervisor task started (interval=%dms, min_heap=%u, stall=%dms)",
             SUPERVISOR_CHECK_INTERVAL_MS,
             (unsigned)SUPERVISOR_MIN_HEAP_BYTES,
             SUPERVISOR_STALL_TIMEOUT_MS);

    uint32_t last_i2s_count = 0;
    uint32_t last_tx_count = 0;
    /* FIX (GROK-3.3): track per-counter "last progress" ticks instead of a
     * single last_check_tick. The old code computed `since_last = now -
     * last_check_tick` which was ALWAYS ~SUPERVISOR_CHECK_INTERVAL_MS (2s)
     * because last_check_tick was updated at the END of every iteration.
     * The TX-only stall condition `since_last >= SUPERVISOR_STALL_TIMEOUT_MS
     * (15s)` was therefore NEVER true → dead code. Only the "both counters
     * stalled" branch (which didn't depend on since_last) could fire.
     *
     * Now: last_i2s_progress_tick and last_tx_progress_tick are updated ONLY
     * when their respective counter advances. The age check `now - last_X_progress_tick
     * >= 15s` then correctly detects a TX-only stall (or I2S-only stall). */
    TickType_t last_i2s_progress_tick = xTaskGetTickCount();
    TickType_t last_tx_progress_tick = xTaskGetTickCount();

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(SUPERVISOR_CHECK_INTERVAL_MS));

        TickType_t now = xTaskGetTickCount();

        /* ---- Check 1: Heap ---- */
        uint32_t free_heap = esp_get_free_heap_size();
        if (free_heap < SUPERVISOR_MIN_HEAP_BYTES)
        {
            ESP_LOGE(TAG, "SUPERVISOR: free heap %u < %u — REBOOT",
                     (unsigned)free_heap, (unsigned)SUPERVISOR_MIN_HEAP_BYTES);
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }

        /* ---- Check 2: Pipeline liveness ---- */
        if (streaming_is_active())
        {
            TickType_t stream_elapsed = now - s_supervisor_stream_start_tick;

            /* Only check after grace period has passed since stream start.
             * During startup, counters may not move for legitimate reasons
             * (I2S DMA fill, WiFi connect, etc.). */
            if (stream_elapsed >= pdMS_TO_TICKS(SUPERVISOR_STALL_TIMEOUT_MS))
            {
                uint32_t cur_i2s = s_supervisor_i2s_count;
                uint32_t cur_tx = s_supervisor_tx_count;

                bool i2s_advanced = (cur_i2s != last_i2s_count);
                bool tx_advanced = (cur_tx != last_tx_count);

                /* FIX (GROK-3.3): update per-counter progress ticks only when
                 * that counter actually advanced. This makes the age-based
                 * partial-stall detection below actually work. */
                if (i2s_advanced)
                    last_i2s_progress_tick = now;
                if (tx_advanced)
                    last_tx_progress_tick = now;

                if (!i2s_advanced && !tx_advanced)
                {
                    /* Neither counter moved since last check.
                     * Pipeline is fully deadlocked. */
                    ESP_LOGE(TAG, "SUPERVISOR: pipeline stalled — "
                             "I2S=%u (was %u), TX=%u (was %u), "
                             "stream_elapsed=%ums — REBOOT",
                             (unsigned)cur_i2s, (unsigned)last_i2s_count,
                             (unsigned)cur_tx, (unsigned)last_tx_count,
                             (unsigned)(stream_elapsed * portTICK_PERIOD_MS));
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_restart();
                }

                /* Partial deadlock: I2S producing frames but TX not sending.
                 * TX task is stuck (sendto timeout, transport not ready, etc.).
                 * FIX (GROK-3.3): use last_tx_progress_tick age instead of
                 * since_last (which was always ~2s, making this dead code).
                 *
                 * FIX (B6 REGRESSION): the GROK-3.3 fix reactivated this
                 * branch, but it falsely reboots when a TCP client hasn't
                 * connected yet (transport_is_ready()==false → TX counter
                 * stays at 0 while I2S produces frames → 15s later REBOOT).
                 * Now we only fire if TX has EVER progressed (cur_tx > 0),
                 * meaning the transport was ready at least once and then
                 * stalled. A stream that never sent a single packet is in
                 * the "waiting for client" phase, not a stall. */
                TickType_t tx_stall_age = now - last_tx_progress_tick;
                if (i2s_advanced && !tx_advanced &&
                    cur_tx > 0 &&
                    tx_stall_age >= pdMS_TO_TICKS(SUPERVISOR_STALL_TIMEOUT_MS))
                {
                    ESP_LOGE(TAG, "SUPERVISOR: TX stalled but I2S active — "
                             "I2S=%u (was %u), TX=%u (was %u), "
                             "tx_stall_age=%ums — REBOOT",
                             (unsigned)cur_i2s, (unsigned)last_i2s_count,
                             (unsigned)cur_tx, (unsigned)last_tx_count,
                             (unsigned)(tx_stall_age * portTICK_PERIOD_MS));
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_restart();
                }
                /* FIX (B6): if TX has never progressed (cur_tx == 0), log a
                 * diagnostic but DON'T reboot — the transport may still be
                 * waiting for a client to connect (TCP) or for WiFi to
                 * settle. The TX task has its own 30s transport-wait timeout
                 * that will signal SVC_ERR_NETWORK if the client never comes. */
                if (i2s_advanced && !tx_advanced && cur_tx == 0 &&
                    tx_stall_age >= pdMS_TO_TICKS(SUPERVISOR_STALL_TIMEOUT_MS))
                {
                    ESP_LOGW(TAG, "SUPERVISOR: TX idle (no client yet?), "
                             "I2S=%u active, TX=%u — waiting (not rebooting)",
                             (unsigned)cur_i2s, (unsigned)cur_tx);
                    /* Reset last_tx_progress_tick so we don't log this every 2s */
                    last_tx_progress_tick = now;
                }

                /* Symmetric: I2S stalled but TX still sending (rare — TX task
                 * recycling old buffers?). Also detect via last_i2s_progress_tick. */
                TickType_t i2s_stall_age = now - last_i2s_progress_tick;
                if (!i2s_advanced && tx_advanced &&
                    i2s_stall_age >= pdMS_TO_TICKS(SUPERVISOR_STALL_TIMEOUT_MS))
                {
                    ESP_LOGE(TAG, "SUPERVISOR: I2S stalled but TX active — "
                             "I2S=%u (was %u), TX=%u (was %u), "
                             "i2s_stall_age=%ums — REBOOT",
                             (unsigned)cur_i2s, (unsigned)last_i2s_count,
                             (unsigned)cur_tx, (unsigned)last_tx_count,
                             (unsigned)(i2s_stall_age * portTICK_PERIOD_MS));
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_restart();
                }

                last_i2s_count = cur_i2s;
                last_tx_count = cur_tx;
            }
        }
        else
        {
            /* Not streaming — reset baselines so next stream start is clean */
            last_i2s_count = s_supervisor_i2s_count;
            last_tx_count = s_supervisor_tx_count;
            last_i2s_progress_tick = xTaskGetTickCount();
            last_tx_progress_tick = xTaskGetTickCount();
        }

        /* ---- Check 3: Stack high-water mark ---- */
        for (int i = 0; i < TASK_IDX_COUNT; i++)
        {
            if (s_task_handles[i])
            {
                UBaseType_t hwm = uxTaskGetStackHighWaterMark(s_task_handles[i]);
                if (hwm < (SUPERVISOR_MIN_STACK_BYTES / sizeof(StackType_t)))
                {
                    ESP_LOGE(TAG, "SUPERVISOR: task %d stack low (%u words) — REBOOT",
                             i, (unsigned)hwm);
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_restart();
                }
            }
        }
    }
}

#endif /* CONFIG_STREAMER_SUPERVISOR_ENABLED */

/* ====================================================================
 * WiFi Boot Retry — connect or deep sleep
 * ====================================================================
 *
 * On boot, tries to connect to the AP WIFI_BOOT_RETRY_ATTEMPTS times.
 * If all attempts fail, enters deep sleep for WIFI_BOOT_SLEEP_MINUTES,
 * then reboots (deep sleep wake = reboot on ESP8266) and retries.
 *
 * Only applies to UDP (transport=0) and TCP (transport=1). RawTX (2)
 * doesn't use AP association — skipped.
 *
 * This prevents the ESP from hanging in a zombie state when the AP is
 * unreachable (power outage, AP reboot, out of range). Without this, the
 * WiFi reconnect task retries forever with exponential backoff, but the
 * ESP never reboots — it just sits there, invisible to the server. */
#if WIFI_BOOT_RETRY_ENABLED
static void wifi_boot_retry_or_sleep(uint8_t transport_mode)
{
    /* RawTX doesn't use AP association — nothing to retry. */
    if (transport_mode != TRANSPORT_MODE_UDP &&
        transport_mode != TRANSPORT_MODE_TCP)
    {
        return;
    }

    ESP_LOGI(TAG, "WiFi boot retry: %d attempts, %ds timeout each, %d min sleep on failure",
             WIFI_BOOT_RETRY_ATTEMPTS,
             (int)(WIFI_CONNECT_TIMEOUT_MS / 1000),
             WIFI_BOOT_SLEEP_MINUTES);

    for (int attempt = 1; attempt <= WIFI_BOOT_RETRY_ATTEMPTS; attempt++)
    {
        if (wifi_sta_is_connected())
        {
            ESP_LOGI(TAG, "WiFi connected on attempt %d/%d",
                     attempt, WIFI_BOOT_RETRY_ATTEMPTS);
            return;
        }

        ESP_LOGW(TAG, "WiFi connect attempt %d/%d (waiting %d ms)...",
                 attempt, WIFI_BOOT_RETRY_ATTEMPTS,
                 (int)WIFI_CONNECT_TIMEOUT_MS);

        esp_err_t err = wifi_sta_wait_connected(WIFI_CONNECT_TIMEOUT_MS);
        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "WiFi connected on attempt %d/%d",
                     attempt, WIFI_BOOT_RETRY_ATTEMPTS);
            return;
        }

        ESP_LOGW(TAG, "WiFi connect attempt %d/%d failed (timeout)",
                 attempt, WIFI_BOOT_RETRY_ATTEMPTS);
    }

    /* All attempts failed — enter sleep. */
    ESP_LOGE(TAG, "WiFi connect failed after %d attempts — entering %s for %d minutes",
             WIFI_BOOT_RETRY_ATTEMPTS,
             (WIFI_BOOT_SLEEP_MODE == 0) ? "deep sleep" : "soft sleep",
             WIFI_BOOT_SLEEP_MINUTES);

#if WIFI_BOOT_SLEEP_MODE == 0
    /* Deep sleep: requires GPIO16 (XPD_DCDC) connected to RST.
     * On wake, ESP reboots → app_main runs → WiFi retry loop repeats. */
    ESP_LOGW(TAG, "Entering deep sleep for %d minutes...", WIFI_BOOT_SLEEP_MINUTES);
    vTaskDelay(pdMS_TO_TICKS(500));  /* let log flush */
    esp_deep_sleep_set_rf_option(2); /* RF cal on wake, don't write NVS */
    esp_deep_sleep((uint64_t)WIFI_BOOT_SLEEP_MINUTES * 60ULL * 1000000ULL);
    /* Should never reach here. */
    ESP_LOGE(TAG, "esp_deep_sleep returned unexpectedly! Restarting...");
    esp_restart();
#else
    /* Soft sleep: works without GPIO16-RST wiring, but uses more power.
     * After the delay, retry WiFi in-place (no reboot). */
    ESP_LOGW(TAG, "Soft sleep (vTaskDelay) for %d minutes, then retry...",
             WIFI_BOOT_SLEEP_MINUTES);
    vTaskDelay(pdMS_TO_TICKS(WIFI_BOOT_SLEEP_MINUTES * 60 * 1000));

    /* After soft sleep, force a WiFi reconnection cycle and reboot
     * to restart cleanly (the WiFi state may be corrupted by now). */
    ESP_LOGW(TAG, "Soft sleep complete — rebooting for clean retry");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
#endif
}
#endif /* WIFI_BOOT_RETRY_ENABLED */

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
        ESP_LOGE(TAG, "Config manager init failed - rebooting in 5s");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }

    /* 3. Stream control EventGroup. */
    s_stream_evt_grp = xEventGroupCreate();
    if (!s_stream_evt_grp)
    {
        ESP_LOGE(TAG, "Failed to create stream event group - rebooting in 5s");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }

    /* 4. AT command interface — started EARLY (before WiFi init) so the user
     * can issue commands even while WiFi is still trying to connect.
     *
     * FIX (AT-DURING-WIFI-RETRY): previously at_cmd_init() was called AFTER
     * wifi_boot_retry_or_sleep(), which is a BLOCKING call that can take up
     * to WIFI_BOOT_RETRY_ATTEMPTS * WIFI_CONNECT_TIMEOUT_MS (tens of seconds)
     * when the AP is unreachable. During that window the AT task did not
     * exist yet, so NO AT commands could be issued — the user was stranded
     * with no way to AT+WIFI=... to fix credentials or AT+RST to reboot.
     * Now AT is started right after config_mgr_init (which AT+ commands
     * depend on) and BEFORE WiFi. Commands that require WiFi (AT+WIFI,
     * AT+STREAM) will return ESP_ERR_INVALID_STATE if WiFi isn't initialized
     * yet — that's correct and expected. Read-only commands (AT+GMR,
     * AT+BATT, AT+HELP, AT+STATUS, AT+FACTORY) work immediately. */
#if AT_CMD_ENABLED
    at_cmd_init();
#endif

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

#if WIFI_BOOT_RETRY_ENABLED
        /* FIX (wifi-boot-retry): On boot, try to connect to the AP multiple
         * times. If all attempts fail, deep sleep + reboot. This prevents
         * the ESP from hanging in a zombie state when the AP is unreachable.
         * Only applies to UDP/TCP (RawTX doesn't use AP association).
         * Replaces the old single wifi_wait_ready call. */
        wifi_boot_retry_or_sleep(cfg.transport_mode);
#else
        /* Block until WiFi is ready (UDP: wait for AP association;
         * RAWTX: no-op). Ignoring errors - start_streaming will retry. */
        stream_mode_ops()->wifi_wait_ready(&cfg);
#endif
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

    /* 7. AT command interface — already initialized in step 4 (before WiFi)
     * so the user can issue commands during WiFi boot retry. Nothing to do
     * here; this comment is kept for historical reference. */

    /* 7.5. Supervisor task — software watchdog.
     * Started BEFORE the main loop so it's always running, even if
     * streaming never starts. Catches heap leaks and stack overflows
     * in any state (IDLE, streaming, transition).
     * Configurable via menuconfig (CONFIG_STREAMER_SUPERVISOR_ENABLED). */
#ifdef CONFIG_STREAMER_SUPERVISOR_ENABLED
    {
        TaskHandle_t h = NULL;
        if (xTaskCreate(supervisor_task_fn, "supervisor",
                        TASK_STACK_SUPERVISOR, NULL,
                        TASK_PRIO_SUPERVISOR, &h) != pdPASS)
        {
            ESP_LOGE(TAG, "Failed to create supervisor task — continuing (no soft-WDT)");
        }
    }
#else
    ESP_LOGI(TAG, "Supervisor task disabled (menuconfig)");
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
    /* FIX (B10): track auto-start retry state. If the initial auto-start
     * (RawTX mode) fails due to transient OOM/I2S error, retry up to 3
     * times with 1/2/5s backoff. Without this, a single boot-time failure
     * leaves the device stuck in idle forever (only manual AT+START or
     * AT+RST could recover). The supervisor will eventually reboot on
     * persistent OOM, but transient failures that free heap on cleanup
     * would not trigger a reboot — leaving the device stranded. */
    int auto_start_attempts = 0;
    const int AUTO_START_MAX_ATTEMPTS = 3;
    const TickType_t auto_start_backoff_ms[3] = {0, 1000, 5000};

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
            /* Stop cancels any pending auto-start retry sequence. */
            auto_start_attempts = 0;
        }
        if (bits & STREAM_EVT_START_REQ)
        {
            esp_err_t start_err = start_streaming();
            /* FIX (B10): if auto-start (RawTX) fails and we haven't exhausted
             * retries, re-arm START_REQ after a backoff delay. This only
             * applies to the initial auto-start sequence (before any manual
             * stop). Once the user manually stops, auto_start_attempts resets
             * to 0 and no further auto-retry happens until reboot. */
            if (start_err != ESP_OK && stream_mode_ops()->auto_start &&
                auto_start_attempts < AUTO_START_MAX_ATTEMPTS)
            {
                auto_start_attempts++;
                TickType_t delay = auto_start_backoff_ms[auto_start_attempts - 1];
                ESP_LOGW(TAG, "Auto-start failed (attempt %d/%d) - retrying in %u ms",
                         auto_start_attempts, AUTO_START_MAX_ATTEMPTS,
                         (unsigned)delay);
                if (delay > 0)
                    vTaskDelay(pdMS_TO_TICKS(delay));
                xEventGroupSetBits(s_stream_evt_grp, STREAM_EVT_START_REQ);
            }
            else if (start_err != ESP_OK)
            {
                ESP_LOGE(TAG, "Auto-start exhausted %d attempts - manual AT+START or AT+RST required",
                         AUTO_START_MAX_ATTEMPTS);
                auto_start_attempts = 0;
            }
            else
            {
                /* Start succeeded — reset retry counter. */
                auto_start_attempts = 0;
            }
        }
    }
}