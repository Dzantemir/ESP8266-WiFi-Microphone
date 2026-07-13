' ============================================================================
'  EASSP Server - Constants and Equates
'  PowerBASIC 10 for Windows
'
'  NOTE: Win32 system equates (%WAVE_FORMAT_PCM, %CALLBACK_NULL, %WHDR_INQUEUE,
'  %WAIT_OBJECT_0, %WAIT_TIMEOUT, %WAIT_FAILED, %INADDR_NONE, etc.) are defined
'  in Win32API.INC (via MMSystem.inc / WinBase.inc).
'  EXCEPTION: %INADDR_NONE and %FD_READ are defined in WINSOCK2.INC /
'  ws2def.inc, which is NOT part of the Win32API.INC include chain. The main
'  file (eassp_server.bas) explicitly $INCLUDE "WINSOCK2.INC" right after
'  Win32API.INC to provide inet_addr, htons, %INADDR_NONE and %FD_READ.
'  Project equates are NOT redefined here - redefinition with the same value is silently accepted by PB,
'  but with a different value it is a hard "Duplicate equate" compile error.
'  Keep this file strictly project-specific.
' ============================================================================

' ---- EASSP Protocol ----
%EASSP_MAGIC0     = &HEA
%EASSP_MAGIC1     = &H55
%EASSP_VER        = 1
%EASSP_HDR_SZ     = 8
%CMD_DISCOVER     = &H01
%CMD_CONFIGURE    = &H02
%CMD_STOP         = &H03   ' Explicit stop — no payload, device stops immediately
%CMD_INFO         = &H81
%STS_IDLE         = 0
%STS_STREAM       = 1
%STS_ERROR        = 2
%INFO_PAYLOAD_SZ  = 58   ' v2.2: +24 bytes hostname (was 33, then 34 in v2.1)
%CFG_PAYLOAD_SZ   = 2
%PKT_HDR_SZ       = 16
%DVI4_HDR_SZ      = 4
%CODEC_ID         = 5
%CODEC_ID_PCM     = 6   ' 16-bit signed PCM (diagnostic, AT+RAW=1 on ESP)

' ---- App Constants ----
%MAX_DEVICES      = 16
%NUM_WAVE_BUFS    = 16
%WAVE_BUF_SZ      = 1920
%HB_INTERVAL      = 3000
%DEV_TIMEOUT_MS   = 30000
%REFRESH_MS       = 2000
' FIX (GROK-10): removed dead %RECV_BUF_SZ equate (was declared but never
' referenced in eassp_server.bas - L35 self-flagged it). Safe to delete.
%AUDIO_PORT_BASE  = 5000

' ---- Stream stall / WiFi-drop recovery ----
' If a streaming device sends no audio packets for this long, HeartbeatThread
' resends CONFIGURE to restart the ESP's stream. Must be > a few heartbeat
' intervals (to avoid false positives during brief congestion) and is
' independent of the ESP's own SVC_WATCHDOG_TIMEOUT_MS (15s): if the ESP
' watchdog fires first, the ESP goes IDLE and only a CONFIGURE can resume it;
' if WiFi comes back before the ESP watchdog, the ESP auto-resumes and the
' stall clears on its own. Resending CONFIGURE is harmless in both cases
' (same-dest CONFIGURE just resets the ESP watchdog; the ESP dedupes).
%STREAM_STALL_MS  = 10000

' ---- "No audio after start" CONFIGURE resend threshold ----
' FIX (BUG#12): previously hardcoded to 3000ms in HeartbeatThread. But
' startup_skip_ms (1s) + fade_in_ms (1s) + socket open + prebuffer easily
' exceeds 3s on slow WiFi / cold ESP boot, causing spurious CONFIGURE resends
' during normal startup (harmless but noisy - extra UDP traffic + log spam).
' 5s only fires on real 'initial CONFIGURE lost' cases.
%NO_AUDIO_RESEND_MS = 5000

' ---- Control IDs ----
%IDC_LISTVIEW     = 101
%IDC_LOG          = 106
%IDC_BTN_START    = 103
%IDC_BTN_STOP     = 104
%IDC_BTN_STOPALL  = 105
%IDC_BTN_DUMP     = 109
%IDC_STATUSBAR    = 108
%IDC_COMBO_DEVICE = 110   ' WaveOut device selection (VB-Cable, etc.)
%IDC_LBL_OUTPUT   = 111   ' "Output:" label for device combo

' ---- Timer / Menu ----
%IDT_REFRESH      = 1
%IDT_DEVCHANGE    = 2    ' Debounce timer for WM_DEVICECHANGE (wait for all audio devices to settle)
%DEVCHANGE_DEBOUNCE_MS = 2000   ' 2s after last WM_DEVICECHANGE before reopening WaveOut
%IDM_ABOUT        = &H1000   ' Must be multiple of 16 (WM_SYSCOMMAND uses low 4 bits)
%IDM_ADD_DEVICE   = &H1010

' ---- ListView context menu (right-click) ----
%IDM_CTX_START     = &H2001
%IDM_CTX_STOP      = &H2002
%IDM_CTX_SELECTALL = &H2003
%IDM_CTX_CLEARALL  = &H2004
%IDM_CTX_STOPALL   = &H2005

' ---- Add Device Dialog ----
%IDD_ADD_DEVICE   = 2000
%IDC_AD_IP        = 2001
%IDC_AD_PORT      = 2002

' ---- UDP notification message ----
%WM_UDP_DISC      = %WM_USER + 500
%WM_UDP_AUDIO     = %WM_USER + 501

' ---- Async log message (PostMessage from worker threads) ----
' FIX C5: AddLog from AudioThread used SendMessage (synchronous) which
' deadlocked when GUI was in WaitForSingleObject. PostMessage is async.
%WM_APP_LOG       = %WM_USER + 600

' ---- PCM Ring Buffer / Jitter depth (ms) / Adaptive ----
' GROK-RINGBUFFER refactor: jitter is now a bytes-level PCM ring buffer
' (single-writer/reader, AudioThread only). The constants below express
' jitter DEPTH in milliseconds, not frames. bytesPerMs is computed from
' the actual WaveOut rate/channels/bits, then target/min/max are derived
' as bytesPerMs * JITTER_*_MS. Ring capacity is fixed at 256KB (enough
' for ~500ms of 48k/16/mono or ~178ms of 48k/24/stereo — comfortably
' above JITTER_MAX_MS=200ms with headroom for bursty WiFi).
' Adaptive is GROW-ONLY (no shrink) per Grok response_4 step 7: simpler,
' avoids oscillation, and the ring drops oldest on overflow so shrink
' isn't needed for correctness.
%JITTER_INITIAL_MS = 120   ' Target ring depth (ms) at stream start / reopen
%JITTER_MIN_MS     = 80   ' Soft-refill threshold after first underrun (ms)
%JITTER_MAX_MS     = 200   ' Adaptive grow ceiling (ms) for bad WiFi
%JITTER_ADAPT_MS   = 30000 ' Adaptive grow check interval (ms)
%JITTER_UNDERRUN_THRESHOLD = 1  ' Underruns in interval to trigger grow
%RING_CAP_BYTES    = 262144   ' 256 KB ring capacity (HeapAlloc'd per device)
' ---- BWSOLA PLC (per Yeh et al. 2013) ----
' Bilateral Waveform Similarity Overlap-Add for Packet Loss Concealment.
' When a packet is lost, BWSOLA reconstructs it using BOTH the previous good
' packet (L_WSOLA) AND the next good packet (R_WSOLA) - hence "bilateral".
' Voiced/unvoiced detection via autocorrelation picks one of 4 strategies:
' BV (both voiced), PV (prev voiced/next unvoiced), NV (mirror), BU (both unvoiced).
' See /tmp/bwsola_original.txt for the paper.
'
' Master switch: 0 = simple frame-repeat PLC (V2 behavior), 1 = BWSOLA PLC.
' When enabled, AudioThread forces 16-bit WaveOut (BWSOLA operates on INTEGER
' PCM; 24-bit ESP PCM is down-converted via the existing >>8 path).
%BWSOLA_PLC_ENABLE    = 1      ' master switch (0 = simple frame-repeat PLC)
%BWSOLA_PITCH_MIN     = 80     ' min pitch period (samples @ 48kHz, per-channel)
%BWSOLA_PITCH_MAX     = 400    ' max pitch period (samples @ 48kHz, per-channel)
%BWSOLA_VOICED_THRESH = 30     ' ACF threshold /100 (0.30) for voiced decision
%BWSOLA_SEARCH_Q      = 4      ' search region = frameLen / Q (paper uses 4)

' ---- Startup click suppression ----
' Skip the first N ms of received audio after stream start. The INMP441 I2S
' MEMS mic has a startup transient (DC offset settling + membrane ringing,
' ~10-15ms) and I2S DMA buffers may contain stale data. Skipping 1 second
' ensures the mic is fully stable before playback begins. The skip consumes
' packets (keeping the socket drained) but discards them before decode/playback.
%STARTUP_SKIP_MS  = 1000

' After the skip completes, apply a linear fade-in over FADE_IN_MS to smooth
' the transition from digital silence (WaveOut buffers) to the real signal.
' This prevents any residual click from the silence→signal boundary (e.g.
' if the mic still has a tiny DC step after 1 second). 1 second fade-in is
' imperceptible to the ear but guarantees a click-free start.
%FADE_IN_MS       = 1000

' ---- String Constants ----
$APP_TITLE        = "EASSP Server - ESP8266 WiFi Microphone"
$DISC_PORT_STR    = "3950"


' ---- ListView Column Indices ----
%LV_COL_MAC       = 0
%LV_COL_IP        = 1
%LV_COL_STATUS    = 2
%LV_COL_RATE      = 3
%LV_COL_BITS      = 4
%LV_COL_CH        = 5
%LV_COL_CODEC     = 6
%LV_COL_RSSI      = 7
%LV_COL_HEAP      = 8
%LV_COL_FW        = 9
%LV_COL_PKTS      = 10
%LV_COL_LOST      = 11
%LV_COL_DUR       = 12
