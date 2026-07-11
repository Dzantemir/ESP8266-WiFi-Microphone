# ESP8266 ADPCM Streamer + Custom I2S Driver — Audit Fixes (Round 3)

This file documents all fixes applied in response to the Grok audits
`grok.txt` (i2s.c-focused) and `grok11.txt` (broader). Each fix is tagged
`GROK-N` or `GROK-G11-N` matching the finding number.

## Files modified

- `i2s.c` (custom I2S driver) — bits_mod comment, DMA alignment asserts, uninstall mux timeout
- `main_extracted/main/wifi_sta.c` — s_backoff_mtx NULL crash fix, init_raw cleanup (raw fixed rate reverted to original `wifi_set_user_fixed_rate`)
- `main_extracted/main/svc_port.c` — s_state race, deinit mutex, clear_error pending
- `main_extracted/main/main.c` — supervisor TX-stall dead code, partial i2s_read underrun detection, AT-during-WiFi-retry fix
- `main_extracted/main/i2s_capture.c` — bytes_per_dma_word 16-bit mono fix
- `main_extracted/main/battery.c` — graceful stream stop before deep sleep
- `main_extracted/main/config_mgr.c` — config_set_* rollback for all functions, hardcoded 20 fix
- `main_extracted/main/at_cmd.c` — overflow discard (don't process truncated commands)
- `main_extracted/main/adpcm_encoder.c` — explicit stdint.h include

## Critical fix (user-reported, not found by Grok)

### AT-DURING-WIFI-RETRY: AT commands unreachable while WiFi boot retry is in progress (CRITICAL)
- **File:** main.c (app_main)
- **Problem:** `at_cmd_init()` was called AFTER `wifi_boot_retry_or_sleep()`, which is a BLOCKING call that can take up to `WIFI_BOOT_RETRY_ATTEMPTS * WIFI_CONNECT_TIMEOUT_MS` (tens of seconds) when the AP is unreachable. During that window the AT task did not exist yet, so NO AT commands could be issued — the user was stranded with no way to `AT+WIFI=...` to fix credentials or `AT+RST` to reboot. Verified on hardware by the user.
- **Fix:** Moved `at_cmd_init()` to BEFORE WiFi init (right after `config_mgr_init` + stream event group). Now the AT task is running before WiFi boot retry starts, so the user can issue commands immediately. Commands that require WiFi (`AT+WIFI`, `AT+STREAM`) return `ESP_ERR_INVALID_STATE` if WiFi isn't initialized yet — credentials are still saved to NVS and applied after reboot. Read-only commands (`AT+GMR`, `AT+BATT`, `AT+HELP`, `AT+STATUS`, `AT+FACTORY`) work immediately.

## Critical fixes

### GROK-G11-#2: s_backoff_mtx NULL → CRASH in raw+AT+WIFI (CRITICAL)
- **File:** wifi_sta.c
- **Problem:** `wifi_sta_init_raw()` did NOT create `s_backoff_mtx` (only `wifi_sta_init()` did). In raw TX mode, `AT+WIFI=...` → `wifi_sta_reconfigure()` → `xSemaphoreTake(s_backoff_mtx, portMAX_DELAY)` on a NULL handle → FreeRTOS dereferences NULL → CRASH.
- **Fix:** (a) Create `s_backoff_mtx` in `wifi_sta_init_raw` too (with cleanup on alloc failure). (b) Add NULL guard in `wifi_sta_reconfigure` as defense-in-depth (if mutex is NULL, reset backoff without lock — safe since raw TX has no reconnect task).

### GROK-3.1: raw fixed rate — REVERTED (Grok was wrong)
- **File:** wifi_sta.c
- **Grok's claim:** `wifi_set_user_fixed_rate(STA, 0x03)` is invalid because 0x03 is not in `enum FIXED_RATE`. Suggested `wifi_set_user_sup_rate(RATE_11B11M, RATE_11B11M)` instead.
- **Why Grok was wrong:** `wifi_set_user_sup_rate()` sets the SUPPORTED RATES advertised to an ASSOCIATED AP (STA mode) — it has NO effect in raw 802.11 TX mode where there is no association. The streamer's raw TX path uses `esp_wifi_80211_tx()` which injects raw frames without any AP association. The correct function to pin the actual TX rate for raw frame injection IS `wifi_set_user_fixed_rate()`. `WIFI_RATE_11M = 0x03` corresponds to the 11 Mbps 802.11b CCK rate.
- **Action:** Reverted to the original `wifi_set_user_fixed_rate(FIXED_RATE_MASK_STA, WIFI_RATE_11M)`. Added a comment explaining why `wifi_set_user_sup_rate` would be WRONG here.

### GROK-3.2: svc_port double-init race (HIGH)
- **File:** svc_port.c
- **Problem:** `svc_port_init` did NOT set `s_state = SVC_IDLE` before returning — it was set only inside `svc_task_fn` after the task started. Window: `svc_port_is_running() == false` immediately after init → second `svc_port_init` passes the guard → duplicate socket + task.
- **Fix:** Set `s_state = SVC_IDLE` under mutex BEFORE `xTaskCreate` in `svc_port_init`. The task's own `s_state = SVC_IDLE` becomes a redundant no-op (safe).

### GROK-3.3: supervisor TX-stall dead code (HIGH)
- **File:** main.c
- **Problem:** `since_last = now - last_check_tick` was ALWAYS ~2s (the check interval) because `last_check_tick` was updated at the END of every iteration. The TX-only stall condition `since_last >= SUPERVISOR_STALL_TIMEOUT_MS (15s)` was NEVER true → dead code. Only the "both counters stalled" branch could fire.
- **Fix:** Replaced single `last_check_tick` with separate `last_i2s_progress_tick` and `last_tx_progress_tick`, updated ONLY when their respective counter advances. Now `now - last_tx_progress_tick >= 15s` correctly detects a TX-only stall. Added symmetric I2S-only stall detection.

### GROK-3.6: wifi_sta_init_raw leaky error paths (HIGH)
- **File:** wifi_sta.c
- **Problem:** 5 error-return paths in `wifi_sta_init_raw` (after `xEventGroupCreate`, `wifi_hw_init`, handler register, `wifi_hw_start`, STA_START timeout, config_err) returned WITHOUT cleanup — leaking `s_wifi_evt`, `s_backoff_mtx`, registered handlers, and/or a half-initialized WiFi stack.
- **Fix:** Refactored with `goto fail_raw_init*` labels that unwind in reverse order: `esp_wifi_stop` (if started) → `esp_event_handler_unregister` (if registered) → `esp_wifi_deinit` (if hw_init succeeded) → `vSemaphoreDelete(s_backoff_mtx)` + `vEventGroupDelete(s_wifi_evt)`.

### GROK-1.6: i2s_driver_uninstall vs stuck reader (HIGH — worse than Grok described)
- **File:** i2s.c
- **Problem:** Grok described "take mux 1s, else proceed". Actually `i2s_driver_uninstall` did NOT take the TX/RX muxes AT ALL — it immediately called `i2s_destroy_dma_queue` which does `vQueueDelete` + `vSemaphoreDelete` on the mux/queue. If a reader was blocked in `i2s_read()` holding `rx->mux` (inside `xQueueReceive(ticks_to_wait)`), the queue and mux were freed out from under it → FreeRTOS undefined behavior (use-after-free, mutex corruption).
- **Fix:** Take TX/RX muxes with a 2s timeout BEFORE destroying the queues. If a mux is held by a blocked reader, refuse uninstall (return `ESP_ERR_TIMEOUT`) so the caller can handle the stuck reader explicitly. The streamer's `wait_for_task_exit` normally ensures the reader has exited, but this closes the HOTRESTART force-delete edge case.

## Medium fixes

### GROK-2.2: bytes_per_dma_word overestimated for 16-bit mono (MED)
- **File:** i2s_capture.c
- **Problem:** `bytes_per_dma_word = (s_bits == 24 && s_channels == 2) ? 8U : 4U` used 4 for ALL 16-bit configs, but 16-bit mono is actually 2 bytes/sample (the custom i2s.c driver allocates `dma_buf_len * sample_size` where `sample_size = bytes_per_sample * channel_num = 2 * 1 = 2`). This overestimated the DMA pool by 2x for 16-bit mono, causing the while-loop to reduce `dma_buf_count` more aggressively than necessary → worse jitter at high sample rates.
- **Fix:** 4-way branch: 16-bit mono = 2U, 16-bit stereo = 4U, 24-bit mono = 4U, 24-bit stereo = 8U (matches the driver's actual `sample_size`).

### GROK-2.3: partial i2s_read zero-pad masks underrun (MED)
- **File:** main.c
- **Problem:** When `i2s_capture_read` returns `ESP_OK` with `n < total`, the missing samples are zero-padded (`for (int i = n; i < total; i++) raw[i] = 0;`) and the frame is sent as if complete — masking the underrun as silence. No underrun counter existed for partial reads (only for full timeouts).
- **Fix:** Added `partial_count` counter. On `n < total`: increment, log (1st + every 50th), and after 5 consecutive partials call `svc_port_set_error(SVC_ERR_I2S)` so the receiver is notified. Reset on a full read.

### GROK-3.5: svc_port_clear_error doesn't clear s_error_pending (MED)
- **File:** svc_port.c
- **Problem:** `svc_port_clear_error` cleared `s_error_code` but NOT `s_error_pending`. After clear_error, the svc_task could flush a stale pending error on its next 100ms iteration — producing an extra (unnecessary) `send_info` and a misleading "flushed pending error N" log. The INFO packet itself contained `error=NONE` (send_info reads s_error_code which was cleared), but the extra traffic + log noise were worth avoiding.
- **Fix:** Added `s_error_pending = SVC_ERR_NONE` to `svc_port_clear_error` under the mutex.

### GROK-3.7: battery deep-sleep without stopping stream (MED)
- **File:** battery.c
- **Problem:** `battery_enter_deep_sleep` called `esp_deep_sleep()` immediately with NO graceful shutdown. TCP/UDP sockets disappeared without FIN/close → server had to time out (cosmetic). No final INFO packet was sent.
- **Fix:** If `streaming_is_active()`, call `streaming_request_stop()` + `vTaskDelay(500ms)` before `esp_deep_sleep()` to let the pipeline flush, send a final frame, and close sockets gracefully. Added `#include "stream_control.h"`.

### GROK-G11-#18: config_set_* rollback — only WiFi had it (MED)
- **File:** config_mgr.c
- **Problem:** `config_set_wifi` had rollback (FIX M21: snapshot old → save → rollback on fail). But 12+ other `config_set_*` functions (tx_power, svc_port, sample_rate, bits, comm_format, channel_format, gain, agc_mode, codec_mode, wifi_channel, transport_mode, i2s_timing, hostname) did NOT rollback on NVS save failure → RAM = new value, NVS = old value → `AT+xxx?` shows new value but reboot loads old value from NVS → "magic rollback" after reboot, confusing the user.
- **Fix:** Applied the M21 rollback pattern to ALL 13 `config_set_*` functions: snapshot old value under mutex, mutate, save, rollback on failure. Added warning log on rollback.

## Low fixes

### GROK-G11-#8: hardcoded 20 in config_mgr ESP_LOGI (LOW)
- **File:** config_mgr.c
- **Problem:** `ESP_LOGI(TAG, "Runtime audio: %u Hz, %d ms, ...", ..., 20, ...)` hardcoded `20` for the frame_ms field, which lied about the actual runtime frame_ms (computed by `i2s_capture_compute_frame_ms`, ranges 5..60ms). The previous `streaming_frame_ms_known()` fix did NOT touch this log line.
- **Fix:** Replaced hardcoded `20` with `streaming_get_frame_ms()` + a "(init, not yet computed)" suffix when `streaming_frame_ms_known()` returns false. Uses `extern` declarations to avoid a hard dependency on stream_control.h in config_mgr.c.

### GROK-G11-#4: svc_port_deinit writes s_state without mutex (LOW)
- **File:** svc_port.c
- **Problem:** `s_state = SVC_STOPPED` in `svc_port_deinit` was written WITHOUT the mutex while `svc_task_fn` reads `s_state` UNDER the mutex — a data race (C11/TSAN would flag it). Practically benign on ESP8266 (32-bit aligned enum = atomic), but inconsistent with the project's discipline.
- **Fix:** Take `s_mutex` for the `s_state` read + write in `svc_port_deinit`. Early-return if mutex is NULL or state is already SVC_STOPPED.

### GROK-G11-#21: adpcm_encoder.c no explicit stdint.h (LOW)
- **File:** adpcm_encoder.c
- **Problem:** Used `int16_t`/`uint8_t`/`int32_t` but did NOT explicitly `#include <stdint.h>` — pulled in transitively via `adpcm_encoder.h` → `esp_err.h` → `stdint.h`. Fragile: refactoring the header could break the .c file.
- **Fix:** Added explicit `#include <stdint.h>`.

### GROK-1.1: BBPLL audio clock conditional on WiFi state — REVERTED (Grok was wrong)
- **File:** i2s.c
- **Grok's claim:** The `if (esp_wifi_get_state() == WIFI_STATE_DEINIT)` guard means the BBPLL enable is skipped when WiFi is up, which is fragile.
- **Why Grok was wrong:** The conditional is INTENTIONAL and CORRECT. WiFi's own init configures the BBPLL for its clock tree (WiFi uses BBPLL). Calling `rom_i2c_writeReg_Mask(0x67, 4, 4, 7, 7, 1)` UNCONDITIONALLY would RE-ENABLE a bit that WiFi may have configured with different parameters, which can destabilize the WiFi radio. The guard ensures we only touch BBPLL when WiFi is NOT running (standalone I2S without WiFi). When WiFi is up, WiFi's BBPLL configuration already provides the audio clock — the streamer empirically works with this arrangement.
- **Action:** Reverted to the original conditional `if (esp_wifi_get_state() == WIFI_STATE_DEINIT)`. Added a comment explaining why unconditional enable would be harmful.

### GROK-1.2: bits_mod ≠ stock SDK (LOW — documentation only)
- **File:** i2s.c
- **Problem:** Custom uses `bits_mod = bits == 16 ? 0 : 8`; stock uses `bits_mod = bits` (16/24). TRM is murky. Grok suggested hardware verification.
- **Finding:** The `0/8` setting is INTENTIONAL and CORRECT for the streamer's LEFT-justified 24-bit layout (low 8 bits = 0x00 padding, extracted via `buf[i] >>= 8`). Already verified on hardware via AT+DUMP hex output. Switching to stock `bits_mod = bits` would BREAK the streamer.
- **Fix:** Added explanatory comment documenting why `0/8` is correct. No code change.

### GROK-1.4: DMA descriptors/buffers without alignment enforcement (LOW)
- **File:** i2s.c
- **Problem:** Used `MALLOC_CAP_8BIT` only; no alignment asserts. `MALLOC_CAP_DMA` is an ESP32 concept (not on ESP8266). ESP8266 heap is always 8-byte aligned, so practically OK.
- **Fix:** Added `assert(((uintptr_t)ptr & 3) == 0)` after each `heap_caps_malloc`/`heap_caps_calloc` for DMA descriptors and buffers. Added `#include <assert.h>`.

### GROK-G11-#16: at_cmd overflow still processed truncated command (LOW)
- **File:** at_cmd.c
- **Problem:** The previous FIX (GROK-25) added a `+ERR` notification on overflow but did NOT discard the truncated buffer — `at_process_line(cmd_buf, pos)` was still called on the next CR/LF with the truncated command. This could parse a valid-but-unintended command (e.g. `AT+RATE=48000,extra` truncated to `AT+RATE=48000` would set the rate to 48000 instead of returning an error).
- **Fix:** Added `overflow_discard` flag. Once overflow is detected, ALL remaining bytes of the current line are discarded, and `at_process_line` is NOT called. The flag resets on CR/LF. The `+ERR` message now says "discarded" instead of "truncated".

## Not changed (intentional)

- **GROK-1.3** (TX ISR owner): Matches stock SDK; streamer is RX-only. No action.
- **GROK-1.5** (mux during xQueueReceive): Architectural deadlock, mitigated by single-reader + finite timeouts + `wait_for_task_exit` before uninstall. The #1.6 fix (uninstall mux timeout) closes the most dangerous edge case. A full drop-mux-before-block refactor is documented but not applied (high risk of regression).
- **GROK-1.7** (TX_START|RX_START): Accepted ESP8266 idiom for master clock. No action.
- **GROK-1.8** (count-1 queue depth): Intentional (matches stock SDK). No action.
- **GROK-2.1** (16-bit swap): Behavior is correct given the custom driver's hardcoded `msb_right=1, right_first=1`. The GROK-16 comment in i2s_capture.c is misleading (claims reliance on SDK default) but the code is correct.
- **GROK-2.4, 2.5** (set_pin before install, timing after install): Already fixed.
- **GROK-3.4** (wifi_sta_reconfigure 200ms wait): Works for common case (different SSID). The `disconnect → wait → set_config → connect` pattern is more robust but higher-risk to refactor. Current approach is acceptable.
- **GROK-3.8-3.14** (force-delete, TOCTOU, endian, frame_ms_known, AT paste, AGC Music, ADPCM): Already fixed or accepted design.
- **GROK-G11-#6** (endianness): Protocol is documented and consistent (stream_port = BE per svc_protocol.h, other fields = LE per packet_format.h). No action.
- **GROK-G11-#12** (seq/timestamp policy): Intentional design per FIX FW#1. ADPCM desync concern is valid but the alternative (always increment seq) would cause unnecessary PLC. No action.

## Summary

- **Critical (fixed):** 4 (GROK-G11-#2, GROK-3.2, GROK-3.3, GROK-3.6, GROK-1.6)
- **Medium (fixed):** 5 (GROK-2.2, GROK-2.3, GROK-3.5, GROK-3.7, GROK-G11-#18)
- **Low (fixed):** 6 (GROK-G11-#8, GROK-G11-#4, GROK-G11-#21, GROK-1.2, GROK-1.4, GROK-G11-#16)
- **REVERTED (Grok was wrong):** 2 (GROK-3.1 raw fixed rate — `wifi_set_user_sup_rate` is for AP-associated STA, wrong for raw TX; GROK-1.1 BBPLL — unconditional enable would destabilize WiFi's clock tree)
- **Not changed (intentional):** 13 findings (documented above)

All fixes are tagged `FIX (GROK-N)` or `FIX (GROK-G11-N)` in the source code with detailed comments.

## Build note

The custom `i2s.c` replaces the stock ESP8266 RTOS SDK `driver/i2s.c`. Place it in the SDK's driver directory (or the project's component directory) so it overrides the stock version. The streamer's `i2s_capture.c` calls the standard `i2s_driver_install`/`i2s_read`/etc. API, which is implemented by this custom driver.
