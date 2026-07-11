/*
 * ESP8266 RTOS SDK I2S capture driver wrapper.
 *
 * Wraps the patched i2s.c driver (BBPLL audio clock + /48 divider for
 * 24-bit). Uses i2s_read() with timeout - proven to work (same approach
 * as esp-i2s-debugger).
 *
 * ВАЖНО: Очередь событий (I2S_EVENT_RX_DONE) НЕ ИСПОЛЬЗУЕТСЯ для чтения.
 * Причина: на ESP8266 RTOS SDK событие RX_DONE приходит раньше, чем данные
 * попадают во внутренний кольцевой буфер rx->queue. i2s_read после RX_DONE
 * возвращает 0 байт (рассинхронизация).
 * i2s_read сама блокирует таску (sleep) и просыпается строго когда данные
 * гарантированно готовы. Это стандартный и самый надёжный способ.
 *
 * 16-bit (rx_fifo_mod=1): 32-bit word = [S_N hi16 | S_N+1 lo16],
 *   pairs swapped on little-endian -> swap back, sign-extend to int32.
 * 24-bit (rx_fifo_mod=3): 32-bit word = [S24 in bits 31:8 | 8 padding in 7:0].
 *   LEFT-justified. Arithmetic >>8 extracts the 24-bit sample and sign-extends
 *   it into int32_t. Verified by AT+DUMP hex output from the ESP itself
 *   (low 8 bits are always 0x00 = padding).
 */

/* ---- System / SDK includes ---- */
#include <string.h>   /* FIX (AUDIT-C3): memcpy used in 16-bit swap/sign-extend loop */
#include "freertos/FreeRTOS.h"
#include "driver/i2s.h"
#include "esp_log.h"
#include "esp8266/i2s_struct.h"
#include "esp8266/eagle_soc.h"

/* ---- Project includes ---- */
#include "board_config.h"
#include "i2s_capture.h"

static const char *TAG = "i2s_cap";
#define I2S_PORT I2S_NUM_0

static bool s_initialized = false;
static int s_bits = 24;
static int s_channels = 1;
static uint32_t s_sample_rate = 16000;
static uint32_t s_frame_ms = 20;
static uint32_t s_dma_buf_ms = 5;
static uint8_t s_gain = 32;        /* 0=bypass, 1..64 = multiplier */
static uint8_t s_agc_mode = 0;     /* 0=OFF, 1..8 = preset index */
static uint8_t s_agc_attack = 75;  /* from preset, % per frame */
static uint8_t s_agc_release = 20; /* from preset, % per frame */
static int32_t s_agc_target = 0;   /* target level (raw, bit-depth dependent) */
static int32_t s_agc_noise_gate = 0; /* noise gate threshold (raw) */
/* FIX (GROK-5): per-preset min-gain floor in Q16.16. Loaded from
 * AGC_PRESETS[mode].min_gain_q16 in i2s_capture_init. Presets 1..6 keep
 * (1<<16)=1.0x (boost-only, historical behavior); Limiter (7) and
 * Surveillance (8) use (1<<10)=1/64x so they can actually attenuate. */
static int32_t s_agc_min_gain_q16 = (1 << 16);
static uint8_t s_timing_sd_delay = 0;
static uint8_t s_timing_ws_delay = 0;
static uint8_t s_timing_bck_delay = 0;

/* AGC state — persists across frames within a stream, reset on init. */
static int32_t s_agc_envelope = 0;         /* peak envelope follower */
static int32_t s_agc_gain_q16 = (1 << 16); /* Q16.16 gain (65536 = 1.0x) */

#define AGC_MAX_GAIN_Q16 (64 << 16) /* 64.0 in Q16.16 = +36 dB */

int i2s_capture_channel_count(int channel_format)
{
    return (channel_format == I2S_CAP_CHFMT_STEREO) ? 2 : 1;
}

/* ---- Frame duration computation ----
 *
 * Goal: pick the largest frame_ms (in preferred set) such that the UDP packet
 * fits in 1400 bytes (MTU safe) and the DMA minimum (32 samples/quarter-frame)
 * is satisfied. Larger frames = less overhead, less CPU, better ADPCM quality.
 *
 * Two hard limits on samples_per_frame:
 *   1. MTU:      pkt = 16 + samples * ch * bytes_per_sample  <= 1400
 *                -> samples <= (1400 - 16) / (ch * bytes_per_sample)
 *   2. DMA min:  dma_buf_len = samples / 4 >= 32  ->  samples >= 128
 * The MTU gives an upper bound; DMA gives a lower bound. We pick the largest
 * preferred ms whose sample count is <= MTU upper bound (and >= 128).
 *
 * ADPCM note: packs 2 samples per byte, so samples must be even. We round
 * the MTU upper bound down to even before checking.
 */
uint32_t i2s_capture_compute_frame_ms(uint32_t sample_rate, int channels,
                                      int codec_mode, int bits_per_sample)
{

    if (sample_rate == 0 || (channels != 1 && channels != 2))
        return 20;

    /* bytes per sample in the packet payload */
    int bps;
    if (codec_mode == 1 /* CODEC_MODE_PCM */)
    {
        bps = (bits_per_sample == 24) ? 3 : 2;
    }
    else
    {
        /* ADPCM: 4 bits/sample = 0.5 bytes/sample, + 4-byte DVI4 header per channel */
        bps = 0; /* handled separately below */
    }

    /* Max samples that fit in MTU (1400 bytes total, 16-byte header). */
    int max_samples;
    if (codec_mode == 1 /* CODEC_MODE_PCM */)
    {
        max_samples = (1400 - 16) / (channels * bps);
    }
    else
    {
        /* ADPCM: 16 + ch*(4 + samples/2) <= 1400
         *      -> samples <= (1400 - 16 - ch*4) * 2 / ch */
        max_samples = ((1400 - 16) - channels * 4) * 2 / channels;
        if (max_samples & 1)
            max_samples--; /* ADPCM needs even */
                           /* Server buffer limit: WAVE_BUF_SZ = 1920 bytes.
                            * ADPCM decode produces 4 bytes per input byte (2 samples x 2 bytes).
                            * Server clips adpcmLen to WAVE_BUF_SZ/4 = 480 bytes per channel.
                            * -> samples <= 480 * 2 = 960 (adpcmLen = samples/2 <= 480).
                            * Without this limit, large frames (e.g. 50ms@48kHz -> adpcmLen=1200)
                            * get truncated by the server -> 60% data loss -> distortion. */
        if (max_samples > 960)
            max_samples = 960;
    }
    if (max_samples < 128)
        max_samples = 128; /* DMA minimum */

    /* Preferred frame durations, largest first - we want the biggest frame
     * that fits, for efficiency and quality. 5ms included for extreme cases. */
    static const uint32_t preferred[] = {60, 50, 40, 30, 25, 20, 15, 10, 5};
    for (int i = 0; i < (int)(sizeof(preferred) / sizeof(preferred[0])); i++)
    {
        uint32_t ms = preferred[i];
        int samples = (int)(sample_rate * ms / 1000);
        if (samples < 128)
            continue; /* DMA minimum */
        if (samples > max_samples)
            continue; /* MTU limit */
        /* Even-sample constraint:
         *   - ADPCM (codec_mode != 1): packs 2 samples per byte, samples MUST
         *     be even. Skip odd.
         *   - PCM: не скипаем нечётные здесь. main.c округляет samples_per_frame
         *     вниз до кратного 8 (16-bit) / 4 (24-bit), что решает и чётность,
         *     и SLC word-alignment, и rw_pos drift. Скипать здесь = выбрать
         *     слишком маленький frame_ms (5ms вместо 15ms для 44100 PCM). */
        if (codec_mode != 1 && (samples & 1))
            continue;
        return ms;
    }

    /* Fallback: scan from max_ms down to 1ms.
     * Only reached when 5ms doesn't fit (e.g. 48kHz stereo PCM-24bit where
     * max_ms=4). No point going above max_ms - nothing larger can fit.
     * Capped at 60ms. */
    int max_ms = max_samples * 1000 / (int)sample_rate;
    if (max_ms > 60)
        max_ms = 60;
    for (int ms = max_ms; ms >= 1; ms--)
    {
        int samples = (int)(sample_rate * ms / 1000);
        if (samples < 128)
            continue;
        if (samples > max_samples)
            continue;
        /* Same even-sample constraint as the preferred loop (ADPCM only). */
        if (codec_mode != 1 && (samples & 1))
            continue;
        return (uint32_t)ms;
    }
    return 20;
}

esp_err_t i2s_capture_init(uint32_t sample_rate, int bits, int comm_format,
                           int channel_format, int samples_per_frame,
                           uint32_t frame_ms, uint8_t gain, uint8_t agc_mode,
                           uint8_t timing_sd_delay, uint8_t timing_ws_delay,
                           uint8_t timing_bck_delay)
{
    if (s_initialized)
        return ESP_ERR_INVALID_STATE;
    if (sample_rate == 0 || (bits != 16 && bits != 24) ||
        (channel_format != I2S_CAP_CHFMT_LEFT &&
         channel_format != I2S_CAP_CHFMT_RIGHT &&
         channel_format != I2S_CAP_CHFMT_STEREO) ||
        /* FIX (L7): validate comm_format. Any value != I2S_CAP_CFMT_LSB
         * previously silently selected Philips - now reject explicitly. */
        (comm_format != I2S_CAP_CFMT_PHILIPS &&
         comm_format != I2S_CAP_CFMT_LSB) ||
        /* FIX (L4): enforce documented DMA minimum (samples_per_frame/4 >=
         * 32 -> samples_per_frame >= 128). The previous < 16 was too
         * lenient; main.c always passes a compute_frame_ms-derived value
         * (>=128) so this is a defensive guard. */
        samples_per_frame < 128 || frame_ms == 0)
        return ESP_ERR_INVALID_ARG;
    if (gain > 64)
        gain = 32;
    if (agc_mode >= AGC_MODE_COUNT)
        agc_mode = AGC_MODE_VOICE_BALANCED;
    if (timing_sd_delay > I2S_TIMING_DELAY_MAX)
        timing_sd_delay = 0;
    if (timing_ws_delay > I2S_TIMING_DELAY_MAX)
        timing_ws_delay = 0;
    if (timing_bck_delay > I2S_TIMING_DELAY_MAX)
        timing_bck_delay = 0;

    s_bits = bits;
    s_channels = i2s_capture_channel_count(channel_format);
    s_sample_rate = sample_rate;
    s_frame_ms = frame_ms;
    s_gain = gain;
    s_agc_mode = agc_mode;
    s_timing_sd_delay  = timing_sd_delay;
    s_timing_ws_delay  = timing_ws_delay;
    s_timing_bck_delay = timing_bck_delay;

    /* Load AGC preset parameters (attack, release, target, noise_gate). */
    {
        const agc_preset_t *p = &AGC_PRESETS[agc_mode];
        s_agc_attack  = p->attack;
        s_agc_release = p->release;
        /* FIX (GROK-5): load per-preset min-gain floor. */
        s_agc_min_gain_q16 = p->min_gain_q16;
        if (s_agc_min_gain_q16 < 1)
            s_agc_min_gain_q16 = 1;  /* defensive: never multiply by 0 */

        /* Compute target and noise_gate for current bit depth.
         * Presets use 0 for target → default (-18 dBFS).
         * Noise gate = target / 64 (if preset has 0). */
        if (s_bits == 24)
        {
            s_agc_target = (1 << 20);       /* -18 dBFS in 24-bit */
            s_agc_noise_gate = s_agc_target / 64;
        }
        else
        {
            s_agc_target = (1 << 12);       /* -18 dBFS in 16-bit */
            s_agc_noise_gate = s_agc_target / 64;
        }

        /* Override with preset-specific target/noise_gate if nonzero.
         * target_q16 is in Q16.16: raw_target = (full_scale * target_q16) >> 16.
         * This works for both 16-bit (full_scale=32768) and 24-bit (8388608)
         * because the preset stores a fraction of full scale, not an absolute
         * raw value. */
        if (p->target_q16 != 0)
        {
            int32_t full_scale = (s_bits == 24) ? 8388608 : 32768;
            s_agc_target = (int32_t)(((int64_t)full_scale * p->target_q16) >> 16);
        }
        if (p->noise_gate_q16 != 0)
        {
            int32_t full_scale = (s_bits == 24) ? 8388608 : 32768;
            s_agc_noise_gate = (int32_t)(((int64_t)full_scale * p->noise_gate_q16) >> 16);
        }
    }

    /* Reset AGC state for fresh stream. */
    s_agc_envelope = 0;
    s_agc_gain_q16 = (1 << 16);

    /* DMA-буфер = 1/4 PCM-кадра -> 4 события RX_DONE на кадр.
     * Это улучшает stop-респонсивность (проверка streaming_is_active() в 4 раза чаще).
     * Драйвер может срезать dma_buf_len если буфер > 4092 байт.
     *
     * samples_per_frame уже выровнено в main.c:
     *   16-bit: кратно 8 → dma_buf_len = samples/4 = чётный → blocksize ≡ 0 (mod 4)
     *   24-bit: кратно 4 → blocksize = dma_buf_len × 4 ≡ 0 (mod 4)
     * Это гарантирует: SLC word-alignment (нет нулевых сэмплов) И
     * want кратен buf_size (нет rw_pos дрейфа, нет потери сэмплов). */
    int dma_buf_len = samples_per_frame / 4;
    if (dma_buf_len < 8)
        dma_buf_len = 8;
    if (dma_buf_len > 1024)
        dma_buf_len = 1024;

    /* Целевая DMA-буферизация: ~80 мс (было 256 мс).
     * I2S - real-time источник, не jitter, поэтому 80 мс более чем достаточно.
     * Экономит ~5 кБ heap на 16kHz/mono (16x320Б=5К вместо 32x320Б=10К).
     * Запас против WiFi-джиттера даёт ADPCM pool, не DMA.
     *
     * count = 80 мс / ms_per_buf = 80 * sample_rate / (1000 * dma_buf_len)
     * + 4 запасных буфера на случай кратковременных задержек.
     *
     * MEMORY CAP: total DMA memory <= 8 KB. At 48kHz, dma_buf_len=240 ->
     * 16 bufs = 15.4 KB (too much). Cap reduces to 8 bufs = 7.5 KB (40ms).
     * This keeps free heap >30 KB even at 48kHz/24-bit/stereo. */
    /* FIX (stereo-mem): bytes_per_dma_word depends on bit depth + channels:
     *   16-bit (any channels): L+R packed in one 32-bit word -> 4 bytes/word
     *   24-bit mono: one 32-bit word per sample -> 4 bytes/word
     *   24-bit stereo: L and R in SEPARATE 32-bit words -> 8 bytes/word
     * Previously the cap used '* 4' unconditionally, which was correct for
     * 16-bit and 24-bit mono but WRONG for 24-bit stereo: it allowed 15.3 KB
     * (8 bufs x 1920) instead of the intended 8 KB, wasting 7 KB of heap.
     * With stereo TCP, this pushed free heap to ~1 KB -> lwIP send() EAGAIN.
     *
     * FIX (GROK-2.2): the 16-bit mono case is actually 2 bytes/sample on
     * the DMA wire (one 16-bit LEFT sample per 32-bit DMA word slot, but
     * the custom i2s.c driver allocates dma_buf_len * sample_size bytes
     * where sample_size = bytes_per_sample * channel_num = 2 * 1 = 2 for
     * 16-bit mono). Using 4 here overestimated the DMA pool by 2x for
     * 16-bit mono, causing the while-loop below to reduce dma_buf_count
     * more aggressively than necessary -> worse jitter at high rates.
     * Now: 16-bit mono = 2U, 16-bit stereo = 4U, 24-bit mono = 4U,
     * 24-bit stereo = 8U (matches the driver's actual sample_size). */
    uint32_t bytes_per_dma_word =
        (s_bits == 24 && s_channels == 2) ? 8U :
        (s_bits == 24 && s_channels == 1) ? 4U :
        (s_bits == 16 && s_channels == 2) ? 4U :
        2U;  /* 16-bit mono */

    int dma_buf_count = (int)(80U * sample_rate / (1000U * (uint32_t)dma_buf_len)) + 4;
    if (dma_buf_count < 6)
        dma_buf_count = 6;
    if (dma_buf_count > 16)
        dma_buf_count = 16;
    while (dma_buf_count > 4 &&
           (uint32_t)dma_buf_count * (uint32_t)dma_buf_len * bytes_per_dma_word > 8192U)
    {
        dma_buf_count--;
    }
    /* FIX (M4): if dma_buf_len is large (samples_per_frame > 1024), the
     * count floor of 4 may still exceed the 8 KB cap. Also reduce
     * dma_buf_len in that case so the total stays under 8 KB. Without
     * this, a caller bypassing compute_frame_ms with a huge
     * samples_per_frame would exceed the cap and exhaust heap. */
    while (dma_buf_len > 8 &&
           (uint32_t)dma_buf_count * (uint32_t)dma_buf_len * bytes_per_dma_word > 8192U)
    {
        dma_buf_len--;
    }

    s_dma_buf_ms = (uint32_t)dma_buf_len * 1000U / sample_rate;

    static const i2s_channel_fmt_t ch_map[5] = {
        [0] = I2S_CHANNEL_FMT_RIGHT_LEFT, // STEREO
        [1] = I2S_CHANNEL_FMT_ALL_RIGHT,  // unused (defensive)
        [2] = I2S_CHANNEL_FMT_ALL_LEFT,   // unused (defensive)
        [3] = I2S_CHANNEL_FMT_ONLY_RIGHT, // RIGHT
        [4] = I2S_CHANNEL_FMT_ONLY_LEFT,  // LEFT
    };

    i2s_config_t cfg = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX,
        .sample_rate = sample_rate,
        .bits_per_sample = (bits == 16) ? I2S_BITS_PER_SAMPLE_16BIT
                                        : I2S_BITS_PER_SAMPLE_24BIT,
        .channel_format = ch_map[channel_format],
        .communication_format = (comm_format == I2S_CAP_CFMT_LSB)
                                    ? (I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_LSB)
                                    : I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB,
        .dma_buf_count = dma_buf_count,
        .dma_buf_len = dma_buf_len,
        .tx_desc_auto_clear = false,
    };

    i2s_pin_config_t pins = {
        .bck_i_en = 1,
        .ws_i_en = 1,
        .data_in_en = 1,
    };

    ESP_LOGI(TAG, "I2S init: %u Hz, %d-bit, %s, %d ch, dma=%dx%d (%lums), gain=%u, agc=%u",
             (unsigned)sample_rate, bits,
             comm_format == I2S_CAP_CFMT_LSB ? "LSB" : "Philips", s_channels,
             dma_buf_count, dma_buf_len, (unsigned long)s_dma_buf_ms,
             (unsigned)s_gain, (unsigned)s_agc_mode);

    /* set_pin before driver_install (patched driver configures GPIO matrix). */
    esp_err_t err = i2s_set_pin(I2S_PORT, &pins);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "i2s_set_pin failed: %s", esp_err_to_name(err));
        return err;
    }

    /* i2s_driver_install: передаём NULL для очереди событий - не нужна.
     * i2s_read сама блокирует таску и просыпается когда данные готовы. */
    err = i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "i2s_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    /* FIX (FW#3b): set s_initialized = true NOW (right after install
     * success), BEFORE the DMA flush and apply_timing. Previously
     * s_initialized was set at the very end of init(), AFTER
     * apply_timing() was called. But apply_timing() checks
     * s_initialized and returns early with a warning if false ->
     * timing delays were NEVER applied. Moving s_initialized here
     * fixes both the warning AND the timing issue. */
    s_initialized = true;

    /* FIX (FW#3): flush stale DMA buffers after i2s_driver_install.
     * After install, the DMA pipeline contains 2-3 buffers of stale data
     * (zeros from init, or residual samples from a previous session if
     * the driver was reinstalled without a reboot). The first
     * i2s_capture_read calls would return this stale data, causing
     * startup clicks that the server's 1-second skip must also cover.
     *
     * The INMP441 MEMS mic also has a startup transient (DC offset
     * settling + membrane ringing, ~10-15ms). Draining 6 small buffers
     * clears the DMA pipeline so only the mic transient remains for
     * the server-side skip to handle.
     *
     * We use a small stack buffer and i2s_read with a short timeout. If
     * the read times out (I2S clock not fully stable), we stop flushing;
     * the server's skip will handle whatever stale data remains. */
    {
        uint8_t flush_buf[256];
        size_t flush_got = 0;
        for (int flush_i = 0; flush_i < 6; flush_i++)
        {
            esp_err_t flush_err = i2s_read(I2S_PORT, flush_buf, sizeof(flush_buf),
                                           &flush_got, pdMS_TO_TICKS(50));
            if (flush_err != ESP_OK || flush_got == 0)
                break;
        }
        ESP_LOGI(TAG, "I2S DMA flush complete (up to 6 buffers drained)");
    }

    /* Apply RX input timing delays (TRM §10.2.1.6, I2S.timing register).
     * Драйвер установлен → регистры I2S доступны. Делаем до старта захвата. */
    i2s_capture_apply_timing(s_timing_sd_delay, s_timing_ws_delay,
                             s_timing_bck_delay);
    if (s_timing_sd_delay || s_timing_ws_delay || s_timing_bck_delay)
    {
        ESP_LOGI(TAG, "I2S RX timing: sd=%u ws=%u bck=%u",
                 (unsigned)s_timing_sd_delay, (unsigned)s_timing_ws_delay,
                 (unsigned)s_timing_bck_delay);
    }

    /* s_initialized already set above (FW#3b) */
    return ESP_OK;
}

void i2s_capture_apply_timing(int sd_delay, int ws_delay, int bck_delay)
{
    /* FIX (AUDIT-H20): bail out if i2s_capture_init() has not been called
     * (or deinit() was just called). Writing to I2S0.timing.* before
     * i2s_driver_install() is undefined (peripheral clock may be off);
     * the value is silently lost on the next init. */
    if (!s_initialized)
    {
        ESP_LOGW(TAG, "i2s_capture_apply_timing ignored - not initialized");
        return;
    }
    /* TRM §10.2.1.6: каждый поле 2 бита (0..3). Маскируем на всякий случай. */
    I2S0.timing.rx_sd_in_delay  = sd_delay  & 0x3;
    I2S0.timing.rx_ws_in_delay  = ws_delay  & 0x3;
    I2S0.timing.rx_bck_in_delay = bck_delay & 0x3;
    /* Memory barrier — гарантируем, что запись завершится до возврата. */
    asm volatile("" ::: "memory");
}

esp_err_t i2s_capture_deinit(void)
{
    if (!s_initialized)
        return ESP_OK;
    /* FIX (L2): propagate the uninstall return so the caller knows if the
     * driver was in a bad state. The next init may otherwise fail. */
    esp_err_t err = i2s_driver_uninstall(I2S_PORT);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "i2s_driver_uninstall: %s - clearing s_initialized anyway "
                 "(caller should reboot if re-init fails)", esp_err_to_name(err));
        /* FIX (GROK-7): clear s_initialized even on uninstall failure.
         * Without this, the next i2s_capture_init() sees s_initialized==true
         * and returns ESP_OK without re-installing the driver — leaving
         * the device stuck in a half-torn-down state where the only
         * recovery is a reboot. Best-effort cleanup is safer than a
         * hard-lock: if the next install genuinely fails because the
         * driver is in a bad state, that error will surface explicitly. */
        s_initialized = false;
        return err;
    }
    s_initialized = false;
    return ESP_OK;
}

/* ---- AGC implementation ----
 * Called after sample extraction (>>8 for 24-bit, sign-extend for 16-bit),
 * before TPDF dither/passthrough. Operates in-place on buf[].
 *
 * 9 presets (AGC_PRESETS[]), each with attack/release/target/noise_gate.
 * Parameters are loaded in i2s_capture_init() based on s_agc_mode. */
static void apply_agc(int32_t *buf, int n)
{
    if (n <= 0)
        return;

    int32_t target_level = s_agc_target;
    int32_t noise_gate = s_agc_noise_gate;
    int32_t max_sample = (s_bits == 24) ? 8388607 : 32767;
    int32_t min_sample = (s_bits == 24) ? -8388608 : -32768;

    /* 1. Find frame peak (absolute max). */
    int32_t frame_peak = 0;
    for (int i = 0; i < n; i++)
    {
        int32_t a = buf[i] < 0 ? -buf[i] : buf[i];
        if (a > frame_peak)
            frame_peak = a;
    }

    /* 2. Update envelope (asymmetric attack/release). */
    if (frame_peak > s_agc_envelope)
    {
        s_agc_envelope = ((100 - (int)s_agc_attack) * s_agc_envelope +
                          (int)s_agc_attack * frame_peak) / 100;
    }
    else
    {
        s_agc_envelope = ((100 - (int)s_agc_release) * s_agc_envelope +
                          (int)s_agc_release * frame_peak) / 100;
    }

    /* 3. Compute target gain. */
    int32_t target_gain_q16;
    /* FIX (M2): use <= so envelope==0 (silence, possible with a future
     * preset that has noise_gate_q16 small enough to produce
     * noise_gate==0 for 16-bit) is caught here instead of falling through
     * to the division by zero below. */
    if (s_agc_envelope <= noise_gate)
    {
        target_gain_q16 = s_agc_min_gain_q16;
    }
    else
    {
        int64_t num = (int64_t)target_level << 16;
        /* FIX (M3): clamp in 64-bit BEFORE the narrowing cast to int32_t.
         * With current presets the ratio is well under INT32_MAX, but a
         * future preset could push it over -> implementation-defined cast
         * before the clamp runs. */
        int64_t raw = num / s_agc_envelope;
        if (raw > AGC_MAX_GAIN_Q16)
            raw = AGC_MAX_GAIN_Q16;
        /* FIX (GROK-5): use the per-preset min-gain floor instead of the
         * historical hardcoded (1<<16). This lets Limiter/Surveillance
         * actually attenuate loud signals instead of clamping to 1.0x
         * and then hard-clipping at the integer range. */
        if (raw < (int64_t)s_agc_min_gain_q16)
            raw = (int64_t)s_agc_min_gain_q16;
        target_gain_q16 = (int32_t)raw;
    }

    /* 4. Smooth gain (prevents zipper noise). */
    if (target_gain_q16 < s_agc_gain_q16)
    {
        s_agc_gain_q16 = ((100 - (int)s_agc_attack) * s_agc_gain_q16 +
                          (int)s_agc_attack * target_gain_q16) / 100;
    }
    else
    {
        s_agc_gain_q16 = ((100 - (int)s_agc_release) * s_agc_gain_q16 +
                          (int)s_agc_release * target_gain_q16) / 100;
    }

    /* 5. Apply gain per-sample + hard limiter. */
    for (int i = 0; i < n; i++)
    {
        int64_t v = (int64_t)buf[i] * (int64_t)s_agc_gain_q16;
        v >>= 16;
        if (v > max_sample)
            v = max_sample;
        if (v < min_sample)
            v = min_sample;
        buf[i] = (int32_t)v;
    }
}

/* ---- Fixed gain implementation ----
 * Applied when AGC is off and s_gain > 0. s_gain=0 means bypass.
 *
 * FIX (AUDIT-GAIN-CONSISTENCY): the previous FIX (M1) bypassed fixed
 * gain entirely in 16-bit mode, claiming 'applying gain=32 saturates any
 * sample |val|>=1023, clipping speech/music'. While technically true,
 * this created an unexpected loudness jump when switching AT+BITS=16
 * <-> AT+BITS=24 with ADPCM: 24-bit mode applied +30 dB, 16-bit mode
 * applied 0 dB. Now fixed gain is applied in BOTH modes with proper
 * clamping.
 *
 * WHY 24-bit PCM sounds louder than 16-bit PCM at the same gain:
 *
 * The signal path differs by codec, not just bit depth:
 *
 *   ADPCM (16 or 24-bit capture):
 *     capture -> apply_fixed_gain -> dither_buffer_24_to_16 OR
 *     dither_buffer_passthrough -> ADPCM encode (always 16-bit domain)
 *     Both bit depths end up in 16-bit domain after dither, so gain=N
 *     produces roughly the same loudness. The dither normalizes the
 *     levels (24-bit samples get >>8 with TPDF noise shaping).
 *
 *   PCM 24-bit:
 *     capture -> apply_fixed_gain -> memcpy int32 DIRECTLY (no dither!)
 *     -> emit 3 bytes/sample -> server plays as 24-bit WaveOut
 *     The full 24-bit dynamic range is preserved. gain=32 can lift a
 *     -30 dBFS signal to 0 dBFS (full 24-bit scale = ±8388607) without
 *     clipping. Server reproduces this at FULL 24-bit amplitude -> very
 *     loud.
 *
 *   PCM 16-bit:
 *     capture -> apply_fixed_gain -> emit 2 bytes/sample -> server
 *     plays as 16-bit WaveOut. gain=32 clips anything >= 1024 (-30 dBFS)
 *     to ±32767 -> heavily distorted AND loud, but with less dynamic
 *     range than 24-bit.
 *
 * Net effect: at the same gain setting, 24-bit PCM is louder AND cleaner
 * because the 24-bit headroom lets gain work without distortion. 16-bit
 * PCM at the same gain is loud but clipped.
 *
 * Recommendation for consistent loudness across bit depths / codecs:
 *   - Use AGC (AT+AGC=3 or higher). AGC targets -18 dBFS in BOTH 16-bit
 *     (1<<12 = 4096) and 24-bit (1<<20 = 1048576) domains, so loudness
 *     is automatically matched.
 *   - For fixed gain: 24-bit PCM use gain=32 (typical); 16-bit PCM use
 *     gain=4-8 (higher causes heavy clipping). ADPCM either is fine
 *     (dither normalizes).
 *   - 24-bit PCM at gain=32 will ALWAYS sound louder than 16-bit PCM at
 *     any gain, because 24-bit preserves 256x more dynamic range. */
static void apply_fixed_gain(int32_t *buf, int n)
{
    if (s_gain == 0)
        return;

    int32_t max_sample = (s_bits == 24) ? 8388607 : 32767;
    int32_t min_sample = (s_bits == 24) ? -8388608 : -32768;

    for (int i = 0; i < n; i++)
    {
        int64_t g = (int64_t)buf[i] * (int64_t)s_gain;
        if (g > max_sample)
            g = max_sample;
        if (g < min_sample)
            g = min_sample;
        buf[i] = (int32_t)g;
    }
}

esp_err_t i2s_capture_read(int32_t *buf, int buf_len, int *samples_read)
{
    if (!s_initialized || !buf || !samples_read || buf_len <= 0)
        return ESP_ERR_INVALID_ARG;
    *samples_read = 0;

    /* Прямой вызов i2s_read - как в debugger (проверено, работает).
     * i2s_read внутри ждёт на rx->queue (внутренняя DMA-очередь).
     * Таймаут: время кадра x 3 + запас. */
    int bytes_per_sample = (s_bits == 16) ? 2 : 4;
    size_t want = (size_t)buf_len * bytes_per_sample;
    size_t got = 0;
    uint32_t read_timeout = s_frame_ms * 3 + 50;

    esp_err_t err = i2s_read(I2S_PORT, buf, want, &got, pdMS_TO_TICKS(read_timeout));
    if (err != ESP_OK)
        return err;
    if (got == 0)
        return ESP_ERR_TIMEOUT;

    int n = (int)(got / bytes_per_sample);

    /* Пост-обработка: extract samples -> AGC or fixed gain. */
    if (s_bits == 16)
    {
        /* 16-bit mode: 32-bit DMA word = [S_N hi16 | S_N+1 lo16].
         * Both halves carry real samples (in ONLY_LEFT mode both slots
         * capture the left channel; in RIGHT_LEFT stereo they carry L/R).
         * Mono: swap pairs (little-endian). Stereo: already interleaved.
         * Then sign-extend int16 -> int32.
         *
         * FIX (AUDIT-C3): the previous code aliased int32_t[] through
         * int16_t* which is a strict-aliasing violation (UB at -O2 with
         * -fstrict-aliasing). Use memcpy + uint32_t scratch instead.
         *
         * FIX (AUDIT-H21): the previous swap loop iterated while
         * i+1<n; for odd n the last sample was not swapped but was
         * still sign-extended (from the wrong DMA half). Round n down
         * to even for the swap; for odd trailing sample, copy the hi16
         * of the last DMA word into the lo16 slot before sign-extend.
         *
         * FIX (GROK-16) DOCUMENTATION ONLY (no behavior change):
         * The ESP8266 I2S driver's default msb_right/right_first
         * configuration determines whether the LEFT-channel sample
         * lands in the hi16 or lo16 of each 32-bit DMA word in stereo
         * mode. We do NOT call i2s_set_clk(...,msb_right=...) here, so
         * we rely on the SDK default for I2S_CHANNEL_FMT_RIGHT_LEFT.
         * If a future SDK version flips that default, the stereo L/R
         * ordering would silently swap on the wire. Receivers that
         * care about L/R identity should validate at stream start
         * (e.g. encode a known L-only or R-only marker frame), or
         * call i2s_set_clk explicitly with the desired msb_right. */
        if (s_channels == 1)
        {
            int n_swap = n & ~1;
            for (int i = 0; i + 1 < n_swap; i += 2)
            {
                /* Read both 16-bit halves of buf[i/2] via memcpy. */
                uint32_t dw;
                memcpy(&dw, &buf[i / 2], sizeof(uint32_t));
                int16_t lo = (int16_t)(dw & 0xFFFFu);
                int16_t hi = (int16_t)((dw >> 16) & 0xFFFFu);
                /* Write back swapped. */
                uint32_t ndw = ((uint32_t)(uint16_t)hi) |
                               ((uint32_t)(uint16_t)lo << 16);
                memcpy(&buf[i / 2], &ndw, sizeof(uint32_t));
            }
            /* For odd n, the last DMA word's hi16 holds the last real
             * sample; lo16 is stale. Move hi16 -> lo16 so the sign-
             * extend loop below picks the right half. */
            if (n & 1)
            {
                uint32_t dw;
                memcpy(&dw, &buf[(n - 1) / 2], sizeof(uint32_t));
                int16_t hi = (int16_t)((dw >> 16) & 0xFFFFu);
                dw = (dw & 0xFFFF0000u) | (uint32_t)(uint16_t)hi;
                memcpy(&buf[(n - 1) / 2], &dw, sizeof(uint32_t));
            }
        }
        /* Sign-extend int16 -> int32, walking from the end so we don't
         * clobber DMA words we still need to read. Use memcpy to read
         * the lo16 of each 32-bit slot without aliasing. */
        for (int i = n - 1; i >= 0; i--)
        {
            uint32_t dw;
            memcpy(&dw, &buf[i / 2], sizeof(uint32_t));
            int16_t v = (i & 1) ? (int16_t)((dw >> 16) & 0xFFFFu)
                                : (int16_t)(dw & 0xFFFFu);
            buf[i] = (int32_t)v;
        }

        /* Apply AGC (if enabled) or fixed gain (if gain > 0).
         * FIX (AUDIT-GAIN-CONSISTENCY): previously fixed gain was bypassed
         * in 16-bit mode (see old FIX M1 comment). Now it's applied in both
         * modes with proper clamping — see apply_fixed_gain() comment for
         * why 24-bit still ends up louder (more headroom survives gain). */
        if (s_agc_mode != AGC_MODE_OFF)
        {
            apply_agc(buf, n);
        }
        else
        {
            apply_fixed_gain(buf, n);
        }
        *samples_read = n;
    }
    else
    {
        /* 24-bit mode: 32-bit DMA word is LEFT-justified -
         * sample in bits [31:8], padding (0x00) in bits [7:0].
         * Arithmetic >>8 extracts the 24-bit sample and sign-extends it
         * into int32_t. Verified by AT+DUMP hex output: low 8 bits
         * are ALWAYS 0x00 (padding), confirming left-justified.
         *
         * After extraction, apply AGC (if enabled) or fixed gain.
         * Gain/AGC operates in 24-bit domain (+/-8388607), before TPDF
         * dither does the 24->16 reduction. */
        for (int i = 0; i < n; i++)
            buf[i] >>= 8;

        if (s_agc_mode != AGC_MODE_OFF)
        {
            apply_agc(buf, n);
        }
        else
        {
            apply_fixed_gain(buf, n);
        }
        *samples_read = n;
    }
    return ESP_OK;
}

int i2s_capture_get_bits(void) { return s_bits; }
