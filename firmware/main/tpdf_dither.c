/*
 * TPDF dither implementation for Xtensa LX106 (ESP8266).
 * See tpdf_dither.h for the input/output contract.
 *
 * IMPLEMENTATION NOTES
 * ====================
 *
 * 1. xorshift32_step() is a PURE function (takes x by value, returns
 *    result - no pointer). With `always_inline` the generated asm is
 *    identical to a pointer-based version; the pure form is stylistically
 *    cleaner and defensive against a future removal of `always_inline`.
 *    Do NOT expect a measurable performance difference - it's about
 *    intent and future-proofing, not cycles.
 *
 * 2. tpdf_state is deliberately NOT volatile. volatile would force a
 *    DRAM load+store on every PRNG step and destroy the register-caching
 *    optimisation that makes this fast. Atomicity comes from 32-bit
 *    aligned access on LX106, NOT from volatile. Do NOT add volatile.
 *
 * 3. Clamp uses two `if` branches, not branchless bit-twiddling. LX106
 *    has no cmov; branches are well-predicted and almost never fire on
 *    real audio, so `if` is cheaper than 4 ALU ops + 2 shifts for a
 *    branchless min/max.
 *
 * 4. No <stdlib.h> - nothing from it is used.
 */

/* ---- System / SDK includes ---- */
#include "esp_attr.h" /* IRAM_ATTR, DRAM_ATTR */

/* ---- Project includes ---- */
#include "tpdf_dither.h"

#ifdef TPDF_DEBUG
#include <assert.h>
/* 24-bit sign-extended in int32_t: top 8 bits all-0 or all-1. */
#define TPDF_CHECK_SIGN_EXT24(v) \
    assert(((v) >> 24) == 0 || ((v) >> 24) == -1)
/* 16-bit sign-extended in int32_t: top 16 bits all-0 or all-1. */
#define TPDF_CHECK_SIGN_EXT16(v) \
    assert(((v) >> 16) == 0 || ((v) >> 16) == -1)
#else
#define TPDF_CHECK_SIGN_EXT24(v) ((void)0)
#define TPDF_CHECK_SIGN_EXT16(v) ((void)0)
#endif

/* PRNG state. Intentionally NOT volatile: caching it in a register
 * across the entire dither loop is the whole point of the optimisation.
 * DO NOT mark this `volatile` "for thread safety" - it will force a
 * DRAM load+store on every sample and tank performance.
 * The function is single-task by contract; the I2S task is the only owner. */
static DRAM_ATTR uint32_t tpdf_state = 0x12345678u;

/* Xorshift32 step. Pure function (value in, value out).
 *
 * Why value-in/value-out and not pointer-to-state:
 * - With `always_inline`, GCC produces identical code for both forms.
 * - The pure form is stylistically cleaner and documents the intent
 *   (no aliasing). It is also defensive: if a future edit removes
 *   `always_inline`, the value form keeps the state in a register
 *   while the pointer form might force a stack spill.
 * - The actual performance is the same; do not expect a measurable
 *   difference on -O2 with always_inline in place. */
static inline IRAM_ATTR __attribute__((always_inline, optimize("O2")))
uint32_t
xorshift32_step(uint32_t x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

void tpdf_init(void)
{
    /* Default deterministic seed. For non-deterministic startup, replace
     * with: tpdf_seed(esp_random()); in app_main() before the I2S task. */
    tpdf_state = 0xdeadbeefu;
}

void tpdf_seed(uint32_t seed)
{
    /* Xorshift32 has a fixed point at 0 - map to 1. */
    tpdf_state = seed ? seed : 1u;
}

/* 24-bit (sign-extended in int32_t) -> 16-bit, with TPDF dither.
 * Input range:  [-8388608, +8388607]
 * Output range: [-32768,  +32767]   (hard-clipped) */
IRAM_ATTR __attribute__((optimize("O2"))) void dither_buffer_24_to_16(const int32_t *in, int16_t *out, int n)
{
    if (n <= 0)
        return;

    /* Cache PRNG state in a CPU register.
     * One DRAM read here, one DRAM write at the end - per buffer. */
    uint32_t s = tpdf_state;

    for (int i = 0; i < n; i++)
    {
        TPDF_CHECK_SIGN_EXT24(in[i]);
        int32_t sample = in[i];

        /* Two independent RPDFs uniform in [0, 255].
         * Difference = TPDF over [-255, +255] = 2 LSB p-p at 16-bit (510/256 = 1.99).
         * Linearises the quantizer, decorrelates error from signal
         * (Wannamaker/Vanderkooy/Lipshitz, 1991). */
        s = xorshift32_step(s);
        uint32_t r1 = s & 0xFFu;
        s = xorshift32_step(s);
        uint32_t r2 = s & 0xFFu;

        int32_t dithered = sample + (int32_t)r1 - (int32_t)r2;

        /* Arithmetic right shift = `srai` on LX106, sign-preserving.
         * Valid ONLY because input is sign-extended - see header. */
        int32_t result = dithered >> 8;

        /* Clamp to int16. Dither can push the result ~1 LSB beyond
         * range; branches almost never fire on real audio. */
        if (result > 32767)
            result = 32767;
        if (result < -32768)
            result = -32768;

        out[i] = (int16_t)result;
    }

    /* Persist PRNG state - single DRAM write per buffer. */
    tpdf_state = s;
}

/* 16-bit sign-extended in int32_t -> int16_t passthrough.
 * Used when the I2S peripheral is configured for 16-bit captures
 * (no bit-depth reduction needed). Clamp guards against I2S bus
 * glitches / overruns producing out-of-range values. */
IRAM_ATTR __attribute__((optimize("O2"))) void dither_buffer_passthrough(const int32_t *in, int16_t *out, int n)
{
    if (n <= 0)
        return;

    for (int i = 0; i < n; i++)
    {
        TPDF_CHECK_SIGN_EXT16(in[i]);
        int32_t v = in[i];
        if (v > 32767)
            v = 32767;
        if (v < -32768)
            v = -32768;
        out[i] = (int16_t)v;
    }
}