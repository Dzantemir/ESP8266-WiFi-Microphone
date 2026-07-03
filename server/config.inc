' ============================================================================
'  EASSP Server - Constants and Equates
'  PowerBASIC 10 for Windows
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
%RECV_BUF_SZ      = 2048
%AUDIO_PORT_BASE  = 5000

' ---- Control IDs ----
%IDC_LISTVIEW     = 101
%IDC_LOG          = 106
%IDC_BTN_START    = 103
%IDC_BTN_STOP     = 104
%IDC_BTN_STOPALL  = 105
%IDC_BTN_DUMP     = 109
%IDC_STATUSBAR    = 108

' ---- Timer / Menu ----
%IDT_REFRESH      = 1
%IDM_ABOUT        = &H1000   ' Must be multiple of 16 (WM_SYSCOMMAND uses low 4 bits)
%IDM_ADD_DEVICE   = &H1010

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

' ---- Wave Constants ----
%WAVE_FORMAT_PCM  = 1
%CALLBACK_NULL    = 0
%WHDR_INQUEUE     = &H10

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
