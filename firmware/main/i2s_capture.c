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
static uint8_t s_agc_mode = 0;     /* 0=OFF, 1=LOW, 2=MEDIUM, 3=HIGH */
static uint8_t s_agc_attack = 75;  /* derived from s_agc_mode at init */
static uint8_t s_agc_release = 20; /* derived from s_agc_mode at init */
static uint8_t s_timing_sd_delay = 0;   /* I2S.timing.rx_sd_in_delay (0..3) */
static uint8_t s_timing_ws_delay = 0;   /* I2S.timing.rx_ws_in_delay (0..3) */
static uint8_t s_timing_bck_delay = 0;  /* I2S.timing.rx_bck_in_delay (0..3) */

/* AGC state - persists across frames within a stream, reset on init.
 *
 * AGC ALGORITHM (speech-optimized):
 *   1. Peak follower with asymmetric attack/release:
 *      - Attack: 75% new per frame -> catches transients in ~20ms (1 frame)
 *      - Release: 10% new per frame -> decays in ~200ms (10 frames)
 *   2. Noise gate: if envelope < target/32 (~-33dB below target = -50dBFS),
 *      bypass AGC (gain=1x) to avoid amplifying silence/noise.
 *   3. Target gain = target_level / envelope, clamped to [1x, 64x]:
 *      - Target = -18 dBFS (12.5% of full scale) - leaves 18dB headroom
 *      - Max gain 64x (+36dB) - limits noise amplification
 *      - Min gain 1x (0dB) - never attenuates (not a compressor)
 *   4. Gain smoothing (asymmetric, prevents zipper noise):
 *      - Reducing (loud transient): 50% per frame -> 95% in ~80ms
 *      - Increasing (quiet section): 10% per frame -> 95% in ~560ms
 *   5. Per-sample: multiply + hard limiter (brick-wall clamp)
 *
 * Works in the native sample domain:
 *   24-bit: after >>8 extraction, range ±8388607, before TPDF dither
 *   16-bit: after sign-extension, range ±32767, before passthrough
 */
static int32_t s_agc_envelope = 0;         /* peak envelope follower */
static int32_t s_agc_gain_q16 = (1 << 16); /* Q16.16 gain (65536 = 1.0x) */

#define AGC_MAX_GAIN_Q16 (64 << 16) /* 64.0 in Q16.16 = +36 dB */
#define AGC_MIN_GAIN_Q16 (1 << 16)  /* 1.0 in Q16.16 = 0 dB */

/* Target levels: -18 dBFS = 12.5% of full scale.
 *   24-bit full scale = 2^23 = 8388608, target = 2^20 = 1048576
 *   16-bit full scale = 2^15 = 32768,  target = 2^12 = 4096
 * Noise gate: target / 64 (about -36 dB below target = -54 dBFS).
 * Low enough to amplify quiet speech (-55 dBFS) but still block mic noise
 * floor (-61 dBFS for INMP441). Was /16 (-42 dBFS) - too aggressive,
 * blocked normal speech from quiet microphone. */
#define AGC_TARGET_24BIT (1 << 20)
#define AGC_TARGET_16BIT (1 << 12)
#define AGC_NOISE_GATE_DIV 64

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
        samples_per_frame < 16 || frame_ms == 0)
        return ESP_ERR_INVALID_ARG;
    if (gain > 64)
        gain = 32;
    if (agc_mode > AGC_MODE_HIGH)
        agc_mode = AGC_MODE_MEDIUM;
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

    /* Resolve AGC preset to attack/release speeds. */
    switch (agc_mode)
    {
    case AGC_MODE_LOW:
        s_agc_attack = AGC_LOW_ATTACK;
        s_agc_release = AGC_LOW_RELEASE;
        break;
    case AGC_MODE_HIGH:
        s_agc_attack = AGC_HIGH_ATTACK;
        s_agc_release = AGC_HIGH_RELEASE;
        break;
    case AGC_MODE_MEDIUM:
    default:
        s_agc_attack = AGC_MED_ATTACK;
        s_agc_release = AGC_MED_RELEASE;
        break;
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
    int dma_buf_count = (int)(80U * sample_rate / (1000U * (uint32_t)dma_buf_len)) + 4;
    if (dma_buf_count < 6)
        dma_buf_count = 6;
    if (dma_buf_count > 16)
        dma_buf_count = 16;
    while (dma_buf_count > 4 &&
           (uint32_t)dma_buf_count * (uint32_t)dma_buf_len * 4U > 8192U)
    {
        dma_buf_count--;
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

    s_initialized = true;
    return ESP_OK;
}

void i2s_capture_apply_timing(int sd_delay, int ws_delay, int bck_delay)
{
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
    i2s_driver_uninstall(I2S_PORT);
    s_initialized = false;
    return ESP_OK;
}

/* ---- AGC implementation ----
 * Called after sample extraction (>>8 for 24-bit, sign-extend for 16-bit),
 * before TPDF dither/passthrough. Operates in-place on buf[].
 *
 * Updates s_agc_envelope and s_agc_gain_q16 state (persists across frames). */
static void apply_agc(int32_t *buf, int n)
{
    if (n <= 0)
        return;

    int32_t target_level = (s_bits == 24) ? AGC_TARGET_24BIT : AGC_TARGET_16BIT;
    int32_t noise_gate = target_level / AGC_NOISE_GATE_DIV;
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

    /* 2. Update envelope (asymmetric attack/release, configurable).
     *    Attack: envelope rises towards peak at s_agc_attack% per frame
     *    Release: envelope falls towards peak at s_agc_release% per frame
     *    Formula: new_env = (100-speed)*old_env + speed*peak, all /100 */
    if (frame_peak > s_agc_envelope)
    {
        s_agc_envelope = ((100 - (int)s_agc_attack) * s_agc_envelope +
                          (int)s_agc_attack * frame_peak) /
                         100;
    }
    else
    {
        s_agc_envelope = ((100 - (int)s_agc_release) * s_agc_envelope +
                          (int)s_agc_release * frame_peak) /
                         100;
    }

    /* 3. Compute target gain.
     *    Noise gate: if envelope below threshold, bypass (gain=1x).
     *    Otherwise: gain = target_level / envelope, clamped [1x, 64x]. */
    int32_t target_gain_q16;
    if (s_agc_envelope < noise_gate)
    {
        /* Signal too quiet - probably noise/silence. Don't amplify. */
        target_gain_q16 = AGC_MIN_GAIN_Q16;
    }
    else
    {
        /* Q16.16 division: (target << 16) / envelope */
        int64_t num = (int64_t)target_level << 16;
        target_gain_q16 = (int32_t)(num / s_agc_envelope);
        if (target_gain_q16 > AGC_MAX_GAIN_Q16)
            target_gain_q16 = AGC_MAX_GAIN_Q16;
        if (target_gain_q16 < AGC_MIN_GAIN_Q16)
            target_gain_q16 = AGC_MIN_GAIN_Q16;
    }

    /* 4. Smooth gain (uses same attack/release speeds, prevents zipper noise).
     *    Reducing (loud transient): fast, proportional to attack speed
     *    Increasing (quiet section): slow, proportional to release speed */
    if (target_gain_q16 < s_agc_gain_q16)
    {
        /* Gain dropping - use attack speed for smoothing */
        s_agc_gain_q16 = ((100 - (int)s_agc_attack) * s_agc_gain_q16 +
                          (int)s_agc_attack * target_gain_q16) /
                         100;
    }
    else
    {
        /* Gain rising - use release speed for smoothing */
        s_agc_gain_q16 = ((100 - (int)s_agc_release) * s_agc_gain_q16 +
                          (int)s_agc_release * target_gain_q16) /
                         100;
    }

    /* 5. Apply gain per-sample + hard limiter (brick-wall clamp). */
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
 * Applied when AGC is off and s_gain > 0. s_gain=0 means bypass. */
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
         * Then sign-extend int16 -> int32. */
        int16_t *s = (int16_t *)buf;
        if (s_channels == 1)
        {
            for (int i = 0; i + 1 < n; i += 2)
            {
                int16_t t = s[i];
                s[i] = s[i + 1];
                s[i + 1] = t;
            }
        }
        for (int i = n - 1; i >= 0; i--)
            buf[i] = (int32_t)s[i];

        /* Apply AGC (if enabled) or fixed gain (if gain > 0). */
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
         * Gain/AGC operates in 24-bit domain (±8388607), before TPDF
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
