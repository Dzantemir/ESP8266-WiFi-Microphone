/*
 * IMA ADPCM encoder - DVI4 streaming format (RFC 3551).
 *
 * IRAM-optimized for ESP8266 Xtensa LX106. The encoder hot path
 * (encode_sample, clamp_int16, clamp_index, adpcm_enc_process) is placed
 * in IRAM to avoid flash cache stalls during WiFi SPI operations.
 *
 * Step table (89 entries) and index table (16 entries) match the
 * reference IMA ADPCM spec exactly. Nibble packing follows RFC 3551
 * DVI4 convention: first sample in high nibble (MSB), second in low
 * nibble (LSB) - opposite of IMA ADPCM WAV format.
 */
#include <stdlib.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "adpcm_encoder.h"

static const char *TAG = "adpcm_enc";

/* IMA ADPCM step table (89 entries, values 7..32767) */
static const DRAM_ATTR int16_t step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

/* IMA ADPCM index table (16 entries: -1,-1,-1,-1,2,4,6,8 repeated) */
static const DRAM_ATTR int8_t index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8};

/* Encoder state (opaque to callers) */
struct adpcm_enc_state
{
    int16_t predictor;
    uint8_t step_index;
};

/* ---- IRAM helper functions ---- */

static inline IRAM_ATTR __attribute__((always_inline, optimize("O2")))
int16_t
clamp_int16(int v)
{
    if (v > 32767)
        return 32767;
    if (v < -32768)
        return -32768;
    return (int16_t)v;
}

static inline IRAM_ATTR __attribute__((always_inline, optimize("O2")))
uint8_t
clamp_index(int v)
{
    if (v < 0)
        return 0;
    if (v > 88)
        return 88;
    return (uint8_t)v;
}

static inline IRAM_ATTR __attribute__((always_inline, optimize("O2")))
uint8_t
encode_sample(adpcm_enc_state_t *s, int16_t sample)
{
    int code = 0;
    int diff = (int)sample - (int)s->predictor;

    if (diff < 0)
    {
        code = 8;     /* bit 3 = sign */
        diff = -diff; /* work with absolute value */
    }

    int step = step_table[s->step_index];

    /* 3-bit magnitude encoding with step/8 bias to mirror the decoder. */
    int delta = step >> 3;

    if (diff >= step)
    {
        code |= 4;
        diff -= step;
        delta += step;
    }
    if (diff >= step / 2)
    {
        code |= 2;
        diff -= step / 2;
        delta += step / 2;
    }
    if (diff >= step / 4)
    {
        code |= 1;
        delta += step / 4;
    }

    if (code & 8)
    {
        s->predictor = clamp_int16((int)s->predictor - delta);
    }
    else
    {
        s->predictor = clamp_int16((int)s->predictor + delta);
    }

    s->step_index = clamp_index((int)s->step_index + index_table[code]);

    return (uint8_t)code;
}

/* ---- Public API ---- */

adpcm_enc_state_t *adpcm_enc_create(void)
{
    adpcm_enc_state_t *s = calloc(1, sizeof(adpcm_enc_state_t));
    if (!s)
    {
        ESP_LOGE(TAG, "Failed to allocate encoder state");
        return NULL;
    }
    s->predictor = 0;
    s->step_index = 0;
    ESP_LOGI(TAG, "ADPCM encoder created (DVI4 RFC 3551, IRAM-optimized)");
    return s;
}

void adpcm_enc_destroy(adpcm_enc_state_t *enc)
{
    if (enc)
    {
        free(enc);
    }
}

IRAM_ATTR __attribute__((optimize("O2"))) esp_err_t adpcm_enc_process(adpcm_enc_state_t *enc,
                                                                      const int16_t *pcm, int num_samples,
                                                                      uint8_t *out, size_t out_size, size_t *written)
{
    if (!enc || !pcm || !out || !written)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (num_samples <= 0 || (num_samples & 1) != 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* Output size = DVI4 header (4 bytes) + ADPCM data (num_samples / 2 bytes) */
    size_t required = sizeof(dvi4_header_t) + (size_t)(num_samples / 2);
    if (out_size < required)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    /* Write DVI4 header (predictor + index + reserved).
     * Predictor is written in CPU native byte order (little-endian on Xtensa).
     * The PB server reads it with CVI() which is also little-endian. */
    out[0] = (uint8_t)(enc->predictor & 0xFF);
    out[1] = (uint8_t)((enc->predictor >> 8) & 0xFF);
    out[2] = (uint8_t)enc->step_index;
    out[3] = 0; /* reserved */

    /* Encode samples to ADPCM nibbles.
     * RFC 3551 DVI4 packing: first sample in HIGH nibble, second in LOW nibble. */
    uint8_t *adpcm_out = out + sizeof(dvi4_header_t);

    for (int i = 0; i < num_samples; i += 2)
    {
        uint8_t nibble_hi = encode_sample(enc, pcm[i]);     /* first -> high nibble */
        uint8_t nibble_lo = encode_sample(enc, pcm[i + 1]); /* second -> low nibble */
        adpcm_out[i / 2] = (uint8_t)(((nibble_hi & 0x0F) << 4) | (nibble_lo & 0x0F));
    }

    *written = required;
    return ESP_OK;
}
