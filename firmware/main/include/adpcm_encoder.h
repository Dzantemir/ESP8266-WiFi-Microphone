#ifndef ADPCM_ENCODER_H
#define ADPCM_ENCODER_H

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

/*
 * IMA ADPCM encoder - DVI4 streaming format.
 *
 * CUSTOM WIRE FORMAT (not strict RFC 3551):
 * - Predictor is written in LITTLE-ENDIAN (native Xtensa order).
 * - State is CONTINUOUS (not reset per packet) - decoder recovers from
 *   the 4-byte header (predictor + index) at the start of each packet.
 * - The 4-byte header includes a reserved byte (0).
 *
 * Thread-safety: NOT thread-safe. Each encoder instance must be used by
 * exactly one task at a time. For stereo, use two separate instances.
 *
 * Each encoded frame includes a 4-byte DVI4 header:
 *   int16_t  predict   - predicted value (decoder state)
 *   uint8_t  index     - step table index (0..88)
 *   uint8_t  reserved  - must be 0
 *
 * Nibble packing: first sample in high nibble (MSB), second in low
 * nibble (LSB). This follows RFC 3551 DVI4 convention.
 */

/* DVI4 block header (4 bytes) - per RFC 3551 Section 4.5.1 */
typedef struct __attribute__((packed)) dvi4_header {
    int16_t  predict;     /* predicted value of first sample (L16) */
    uint8_t  index;       /* current index into step table (0..88) */
    uint8_t  reserved;    /* set to 0 by sender, ignored by receiver */
} dvi4_header_t;

_Static_assert(sizeof(dvi4_header_t) == 4, "dvi4_header_t must be 4 bytes");

/* Opaque encoder state */
typedef struct adpcm_enc_state adpcm_enc_state_t;

/* Initialize ADPCM encoder state. Returns NULL on failure. */
adpcm_enc_state_t *adpcm_enc_create(void);

/* Destroy encoder state and free memory. */
void adpcm_enc_destroy(adpcm_enc_state_t *enc);

/*
 * Encode a frame of 16-bit PCM to DVI4 ADPCM.
 *
 * Output layout (DVI4_HEADER_SIZE + num_samples/2 bytes):
 *   [dvi4_header_t (4 bytes)] [ADPCM nibbles (N/2 bytes)]
 *
 * @param enc         Encoder state
 * @param pcm         Input PCM samples (int16_t)
 * @param num_samples Number of input samples (must be even)
 * @param out         Output buffer for DVI4 block
 * @param out_size    Size of output buffer (>= 4 + num_samples/2)
 * @param written     Output: bytes written
 * @return ESP_OK on success
 */
esp_err_t adpcm_enc_process(adpcm_enc_state_t *enc,
                             const int16_t *pcm, int num_samples,
                             uint8_t *out, size_t out_size, size_t *written);

#endif /* ADPCM_ENCODER_H */
