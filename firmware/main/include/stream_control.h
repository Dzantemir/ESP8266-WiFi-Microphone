#ifndef STREAM_CONTROL_H
#define STREAM_CONTROL_H

/*
 * Stream Control API
 * ==================
 *
 * Public interface for controlling the audio stream pipeline from outside
 * main.c (e.g., from AT command handlers).
 *
 * The main loop in main.c owns the EventGroup and calls start_streaming() /
 * stop_streaming() in response to the event bits set by these functions.
 */

#include <stdbool.h>

/*
 * Check if the stream is currently active (streaming).
 * Returns true if streaming, false if stopped or not initialized.
 */
bool streaming_is_active(void);

/*
 * Request stream stop.
 * Sets the STOP_REQ event bit. The main loop will call stop_streaming().
 */
bool streaming_request_stop(void);

/*
 * Request a hot restart of the stream.
 *
 * Stops the current stream and starts a new one with the current NVS config.
 * This allows audio parameter changes (AT+RATE, AT+BITS, etc.) to take effect
 * without a full device reboot.
 *
 * ONLY works when the stream is currently active. If the stream is already
 * stopped, this does nothing and returns false - the saved params will apply
 * naturally on the next stream start (CONFIGURE from server in UDP mode,
 * or auto-start in Raw TX mode). This prevents HOTRESTART from overriding
 * an intentional CMD_STOP from the server.
 *
 * Returns true if the restart was requested, false if stream is not active
 * or stream control is not initialized.
 */
bool streaming_request_restart(void);

/*
 * Get the current per-frame duration in milliseconds.
 * Updated by start_streaming() based on rate/channels/codec/MTU.
 * Returns the static-init placeholder (20) before the first successful
 * start_streaming() call — callers that need to distinguish "no stream
 * ever started" from "real value" should use streaming_frame_ms_known().
 */
uint32_t streaming_get_frame_ms(void);

/*
 * FIX (GROK-21): returns true once start_streaming() has computed a real
 * frame_ms from the I2S parameters. Before the first successful start,
 * returns false (streaming_get_frame_ms() then returns the static-init
 * placeholder of 20, which is NOT a real runtime value).
 */
bool streaming_frame_ms_known(void);

/*
 * FIX (B3): Get the channel count of the ACTIVE stream.
 * Returns the channel count the running stream is actually using (1 or 2).
 * If no stream is active, returns the configured (NVS) channel count.
 * Use this instead of svc_port's local s_channels for INFO/STATUS packets,
 * so the receiver always sees the actual stream channel count (not a
 * prematurely-updated value from AT+CH before HOTRESTART applies it).
 */
uint8_t streaming_get_channels(void);

#endif /* STREAM_CONTROL_H */
