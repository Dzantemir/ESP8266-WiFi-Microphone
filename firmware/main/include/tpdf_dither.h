#ifndef TPDF_DITHER_H
#define TPDF_DITHER_H

#include <stdint.h>

/*
 * TPDF dither: 24-bit -> 16-bit (and 16-bit passthrough) for I2S audio.
 * Target: Xtensa LX106 (ESP8266), single-core, called from I2S task only.
 *
 * == WHAT ==
 * Adds triangular-PDF noise before 24->16-bit truncation. Linearises
 * the quantizer and decorrelates quantisation error from the signal -
 * the standard Wannamaker / Vanderkooy / Lipshitz (1991) recipe.
 *
 * TPDF = difference of two independent RPDFs uniform in [0, 255],
 * giving a triangular distribution over [-255, +255] - exactly 2 LSB p-p
 * at 16-bit, the mathematically optimal amplitude.
 *
 * == INPUT CONTRACT (verify with your I2S driver) ==
 *
 * dither_buffer_24_to_16():
 *   in[i] MUST be a sign-extended 24-bit sample in int32_t:
 *     - Positive: (in[i] >> 24) == 0
 *     - Negative: (in[i] >> 24) == -1
 *   If your I2S driver delivers 24-bit in the LSBs with zero-padded
 *   upper bits, you MUST sign-extend before calling. Enable TPDF_DEBUG
 *   to assert this at runtime.
 *
 * dither_buffer_passthrough():
 *   in[i] MUST be a sign-extended 16-bit sample in int32_t:
 *     - Positive: (in[i] >> 16) == 0
 *     - Negative: (in[i] >> 16) == -1
 *   Used when I2S is configured for 16-bit captures (no bit-depth
 *   reduction needed). Clamp only guards against I2S bus glitches.
 *
 * == CONCURRENCY ==
 * - dither_buffer_*() must run in a single task (the I2S task).
 * - tpdf_seed() must NOT be called concurrently with dithering.
 * - 32-bit aligned R/W of tpdf_state is atomic on Xtensa LX106.
 *
 * == PERFORMANCE ==
 * - Hot functions live in IRAM (no flash cache stalls during WiFi SPI).
 * - PRNG state is cached in a CPU register across the whole loop.
 * - DRAM traffic: 1 read + 1 write of PRNG state per *buffer*,
 *   not per sample. Do NOT add `volatile` to tpdf_state - it would
 *   force a DRAM access on every sample and destroy the optimisation.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise PRNG with a fixed, reproducible seed. Call once at startup.
 * For non-deterministic startup, call tpdf_seed(esp_random()) instead. */
void tpdf_init(void);

/* Seed the PRNG. A zero seed is mapped to 1 (xorshift32 has a fixed
 * point at 0). Must NOT be called concurrently with dithering. */
void tpdf_seed(uint32_t seed);

/* 24-bit (sign-extended in int32_t) -> 16-bit, with TPDF dither.
 * Input range:  [-8388608, +8388607]
 * Output range: [-32768,    +32767]   (hard-clipped) */
void dither_buffer_24_to_16(const int32_t *in, int16_t *out, int n);

/* 16-bit sign-extended in int32_t -> int16_t passthrough.
 * No bit-depth reduction - clamp + narrowing copy only. */
void dither_buffer_passthrough(const int32_t *in, int16_t *out, int n);

#ifdef __cplusplus
}
#endif

#endif /* TPDF_DITHER_H */