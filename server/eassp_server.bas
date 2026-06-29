' ============================================================================
'  EASSP Server - ESP8266 WiFi Microphone
'  PowerBASIC 10 for Windows
'
'  Protocol: EASSP over UDP port 3950
'  Audio: IMA ADPCM (DVI4/RFC3551) -> 16-bit PCM, sample rate from device
'  Playback: WaveOut API
'  GUI: DDT + ListView (dynamic add/remove) + StatusBar, resizable
'
'  Uses PowerBASIC built-in:
'    - DIALOG NEW / CONTROL ADD / CALLBACK FUNCTION (DDT)
'    - UDP OPEN / UDP RECV / UDP SEND / UDP CLOSE
'    - UDP NOTIFY for async receive
'    - THREAD CREATE / THREAD FUNCTION
'    - HI() / LO() / MAK() functions
'
'  Compile:  PB/Win IDE or command-line
'  Output:   eassp_server.exe
' ============================================================================

#COMPILE EXE
#DIM ALL
#OPTION VERSION5

' ---- Win32 API includes ----
$INCLUDE "WIN32API.INC"

' ---- Project includes ----
$INCLUDE "config.inc"
$INCLUDE "types.inc"

' ============================================================================
'  GLOBAL VARIABLES
' ============================================================================
GLOBAL g_hDlg       AS DWORD
GLOBAL g_hHeap      AS DWORD
GLOBAL g_fDiscFile  AS LONG     ' UDP file number for discovery
GLOBAL g_fDiscOpen  AS LONG     ' is discovery socket open?
GLOBAL g_hHbTh      AS DWORD    ' heartbeat thread handle
GLOBAL g_bRunning   AS LONG
GLOBAL g_seqCnt     AS LONG     ' was WORD, changed to LONG for InterlockedIncrement

GLOBAL g_csDev      AS CRITICAL_SECTION

GLOBAL g_Devs()     AS DeviceInfo

' ---- Raw dump mode (diagnostic) ----
' When g_bDumping <> 0, the audio thread writes decoded PCM audio to a WAV
' file. WAV header is written on first packet using format from the packet.
' Files auto-split at 1 GB to avoid filesystem limits.
' Toggled by the DUMP button.
GLOBAL g_bDumping   AS LONG
GLOBAL g_hDumpFile  AS LONG
GLOBAL g_dumpCodec  AS LONG
GLOBAL g_dumpIp     AS DWORD        ' IP of device being dumped (0 = any)
GLOBAL g_csDump     AS CRITICAL_SECTION
' WAV-specific state
GLOBAL g_dumpWavReady   AS LONG     ' 0=header not written yet, 1=header written
GLOBAL g_dumpSampleRate AS DWORD    ' Hz (from packet)
GLOBAL g_dumpChannels   AS LONG     ' 1 or 2
GLOBAL g_dumpBits       AS LONG     ' 16 or 24 (PCM), 16 (ADPCM decoded)
GLOBAL g_dumpDataSize   AS QUAD     ' bytes written to data chunk
GLOBAL g_dumpFileIdx    AS LONG     ' file split index (1, 2, 3...)
GLOBAL g_dumpBaseName   AS STRING   ' base filename without extension
GLOBAL g_dumpSeqCounter AS LONG     ' to detect format change mid-dump

' g_wfFormat removed - was unused (each AudioThread creates its own local wfFmt)

GLOBAL g_StepTable() AS LONG
GLOBAL g_IndexTable() AS LONG

' ============================================================================
'  UTILITY FUNCTIONS
' ============================================================================

' FormatIP - Convert network-order DWORD IP to "a.b.c.d"
FUNCTION FormatIP(BYVAL dwIP AS DWORD) AS STRING
    LOCAL p AS BYTE PTR
    p = VARPTR(dwIP)
    FUNCTION = USING$("#_.#_.#_.#", @p, @p[1], @p[2], @p[3])
END FUNCTION

' AddLog - Append timestamped message to log textbox
SUB AddLog(BYVAL sText AS STRING)
    LOCAL st AS SYSTEMTIME
    LOCAL sLine AS STRING

    GetLocalTime st

    sLine = "[" & RIGHT$("0" & TRIM$(STR$(st.wHour)), 2) & ":" & _
                  RIGHT$("0" & TRIM$(STR$(st.wMinute)), 2) & ":" & _
                  RIGHT$("0" & TRIM$(STR$(st.wSecond)), 2) & "] " & _
            sText & $CRLF

    ' FIX C5: Use PostMessage (async) instead of CONTROL SEND (sync SendMessage).
    ' SendMessage from worker threads deadlocks when GUI is in WaitForSingleObject
    ' during shutdown → 3s timeout → orphan thread → DeleteCriticalSection
    ' use-after-free → crash. PostMessage never blocks.
    ' We pass STRPTR(sLine) - safe because the string handle stays alive until
    ' the GUI thread processes WM_APP_LOG and reads it. PowerBASIC strings use
    ' reference counting, so the handle remains valid as long as the PostMessage
    ' payload references it.
    PostMessage g_hDlg, %WM_APP_LOG, 0, STRPTR(sLine)
END SUB

' AddLogSync - synchronous AddLog for use from GUI thread only (avoids
' PostMessage queue overflow for high-volume logging from GUI).
SUB AddLogSync(BYVAL sText AS STRING)
    LOCAL st AS SYSTEMTIME
    LOCAL sLine AS STRING

    GetLocalTime st

    sLine = "[" & RIGHT$("0" & TRIM$(STR$(st.wHour)), 2) & ":" & _
                  RIGHT$("0" & TRIM$(STR$(st.wMinute)), 2) & ":" & _
                  RIGHT$("0" & TRIM$(STR$(st.wSecond)), 2) & "] " & _
            sText & $CRLF

    LOCAL nLen AS LONG
    CONTROL SEND g_hDlg, %IDC_LOG, %WM_GETTEXTLENGTH, 0, 0 TO nLen
    IF nLen > 32768 THEN
        CONTROL SET TEXT g_hDlg, %IDC_LOG, ""
        nLen = 0
    END IF
    CONTROL SEND g_hDlg, %IDC_LOG, %EM_SETSEL, nLen, nLen
    CONTROL SEND g_hDlg, %IDC_LOG, %EM_REPLACESEL, 0, STRPTR(sLine)
END SUB

' InitStepTable - Initialize IMA ADPCM lookup tables
SUB InitStepTable()
    ARRAY ASSIGN g_StepTable() = _
        7, 8, 9, 10, 11, 12, 13, 14, _
        16, 17, 19, 21, 23, 25, 28, 31, _
        34, 37, 41, 45, 50, 55, 60, 66, _
        73, 80, 88, 97, 107, 118, 130, 143, _
        157, 173, 190, 209, 230, 253, 279, 307, _
        337, 371, 408, 449, 494, 544, 598, 658, _
        724, 796, 876, 963, 1060, 1166, 1282, 1411, _
        1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, _
        3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, _
        7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, _
        15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, _
        32767

    ARRAY ASSIGN g_IndexTable() = -1, -1, -1, -1, 2, 4, 6, 8
END SUB

' ============================================================================
'  LISTVIEW HELPERS
' ============================================================================

' InitListView - Create columns in the ListView
SUB InitListView()
    LOCAL lvc AS LV_COL
    LOCAL szText AS ASCIIZ * 64

    lvc.mask = %LVCF_FMT OR %LVCF_WIDTH OR %LVCF_TEXT
    lvc.fmt  = %LVCFMT_LEFT

    ' Col 0 - MAC
    szText = "MAC"        : lvc.pszText = VARPTR(szText) : lvc.cx = 130
    CONTROL SEND g_hDlg, %IDC_LISTVIEW, %LVM_INSERTCOLUMN, %LV_COL_MAC, VARPTR(lvc)

    ' Col 1 - IP:Port
    szText = "IP:Port"    : lvc.pszText = VARPTR(szText) : lvc.cx = 135
    CONTROL SEND g_hDlg, %IDC_LISTVIEW, %LVM_INSERTCOLUMN, %LV_COL_IP, VARPTR(lvc)

    ' Col 2 - Status
    szText = "Status"     : lvc.pszText = VARPTR(szText) : lvc.cx = 75
    CONTROL SEND g_hDlg, %IDC_LISTVIEW, %LVM_INSERTCOLUMN, %LV_COL_STATUS, VARPTR(lvc)

    ' Col 3 - Sample Rate
    szText = "Rate"       : lvc.pszText = VARPTR(szText) : lvc.cx = 55
    CONTROL SEND g_hDlg, %IDC_LISTVIEW, %LVM_INSERTCOLUMN, %LV_COL_RATE, VARPTR(lvc)

    ' Col 4 - Bits
    szText = "Bits"       : lvc.pszText = VARPTR(szText) : lvc.cx = 40
    CONTROL SEND g_hDlg, %IDC_LISTVIEW, %LVM_INSERTCOLUMN, %LV_COL_BITS, VARPTR(lvc)

    ' Col 5 - Channels
    szText = "Ch"         : lvc.pszText = VARPTR(szText) : lvc.cx = 35
    CONTROL SEND g_hDlg, %IDC_LISTVIEW, %LVM_INSERTCOLUMN, %LV_COL_CH, VARPTR(lvc)

    ' Col 6 - Codec
    szText = "Codec"      : lvc.pszText = VARPTR(szText) : lvc.cx = 60
    CONTROL SEND g_hDlg, %IDC_LISTVIEW, %LVM_INSERTCOLUMN, %LV_COL_CODEC, VARPTR(lvc)

    ' Col 7 - RSSI
    szText = "RSSI"       : lvc.pszText = VARPTR(szText) : lvc.cx = 60
    CONTROL SEND g_hDlg, %IDC_LISTVIEW, %LVM_INSERTCOLUMN, %LV_COL_RSSI, VARPTR(lvc)

    ' Col 8 - Heap
    szText = "Heap"       : lvc.pszText = VARPTR(szText) : lvc.cx = 60
    CONTROL SEND g_hDlg, %IDC_LISTVIEW, %LVM_INSERTCOLUMN, %LV_COL_HEAP, VARPTR(lvc)

    ' Col 9 - Firmware
    szText = "FW"         : lvc.pszText = VARPTR(szText) : lvc.cx = 55
    CONTROL SEND g_hDlg, %IDC_LISTVIEW, %LVM_INSERTCOLUMN, %LV_COL_FW, VARPTR(lvc)

    ' Col 10 - Packets
    szText = "Pkts"       : lvc.pszText = VARPTR(szText) : lvc.cx = 60
    CONTROL SEND g_hDlg, %IDC_LISTVIEW, %LVM_INSERTCOLUMN, %LV_COL_PKTS, VARPTR(lvc)

    ' Col 11 - Lost
    szText = "Lost"       : lvc.pszText = VARPTR(szText) : lvc.cx = 50
    CONTROL SEND g_hDlg, %IDC_LISTVIEW, %LVM_INSERTCOLUMN, %LV_COL_LOST, VARPTR(lvc)

    ' Col 12 - Duration
    szText = "Duration"   : lvc.pszText = VARPTR(szText) : lvc.cx = 70
    CONTROL SEND g_hDlg, %IDC_LISTVIEW, %LVM_INSERTCOLUMN, %LV_COL_DUR, VARPTR(lvc)

    ' Extended styles: full row select + grid lines + checkboxes + double buffer
    ' LVS_EX_DOUBLEBUFFER prevents flicker during rapid updates (ComCtl32 v6+)
    CONTROL SEND g_hDlg, %IDC_LISTVIEW, %LVM_SETEXTENDEDLISTVIEWSTYLE, _
        %LVS_EX_FULLROWSELECT OR %LVS_EX_GRIDLINES OR %LVS_EX_CHECKBOXES OR %LVS_EX_DOUBLEBUFFER, _
        %LVS_EX_FULLROWSELECT OR %LVS_EX_GRIDLINES OR %LVS_EX_CHECKBOXES OR %LVS_EX_DOUBLEBUFFER
END SUB

' GetSelectedDevIdx - Get device array index for selected ListView item
FUNCTION GetSelectedDevIdx() AS LONG
    LOCAL lvIdx AS LONG
    LOCAL lvi AS LV_ITM
    LOCAL lResult AS LONG

    CONTROL SEND g_hDlg, %IDC_LISTVIEW, %LVM_GETNEXTITEM, -1, _
        MAK(LONG, %LVNI_SELECTED, 0) TO lvIdx

    IF lvIdx < 0 THEN
        FUNCTION = -1
        EXIT FUNCTION
    END IF

    lvi.mask   = %LVIF_PARAM
    lvi.iItem  = lvIdx
    lvi.iSubItem = 0
    CONTROL SEND g_hDlg, %IDC_LISTVIEW, %LVM_GETITEM, 0, VARPTR(lvi) TO lResult

    FUNCTION = lvi.lParam
END FUNCTION

' IsItemChecked - Check if ListView item checkbox is checked
FUNCTION IsItemChecked(BYVAL hLV AS DWORD, BYVAL iItem AS LONG) AS LONG
    LOCAL dwState AS DWORD
    dwState = SendMessage(hLV, %LVM_GETITEMSTATE, iItem, %LVIS_STATEIMAGEMASK)
    ' State image index 2 = checked → bits 12-15 = 2 → &H2000
    FUNCTION = (dwState AND &H2000) = &H2000
END FUNCTION

' UpdateButtonStates - Enable/disable buttons based on checkbox & stream state
SUB UpdateButtonStates()
    LOCAL hLV AS DWORD
    LOCAL i AS LONG
    LOCAL nCount AS LONG
    LOCAL bCanStart AS LONG
    LOCAL bCanStop AS LONG
    LOCAL bHasStream AS LONG
    LOCAL devIdx AS LONG
    LOCAL lvi AS LV_ITM

    CONTROL HANDLE g_hDlg, %IDC_LISTVIEW TO hLV
    IF hLV = 0 THEN EXIT SUB

    nCount = SendMessage(hLV, %LVM_GETITEMCOUNT, 0, 0)

    ' Read g_Devs under CS to prevent data races
    EnterCriticalSection g_csDev

    FOR i = 0 TO nCount - 1
        lvi.mask     = %LVIF_PARAM
        lvi.iItem    = i
        lvi.iSubItem = 0
        SendMessage hLV, %LVM_GETITEM, 0, VARPTR(lvi)
        devIdx = lvi.lParam

        IF devIdx < 0 OR devIdx >= %MAX_DEVICES THEN ITERATE FOR

        IF IsItemChecked(hLV, i) THEN
            IF g_Devs(devIdx).dwActive AND g_Devs(devIdx).dwHBActive = 0 THEN bCanStart = 1
            IF g_Devs(devIdx).dwHBActive THEN bCanStop = 1
        END IF

        IF g_Devs(devIdx).dwHBActive THEN bHasStream = 1
    NEXT i

    LeaveCriticalSection g_csDev

    IF bCanStart  THEN CONTROL ENABLE  g_hDlg, %IDC_BTN_START   ELSE CONTROL DISABLE g_hDlg, %IDC_BTN_START
    IF bCanStop   THEN CONTROL ENABLE  g_hDlg, %IDC_BTN_STOP    ELSE CONTROL DISABLE g_hDlg, %IDC_BTN_STOP
    IF bHasStream THEN CONTROL ENABLE  g_hDlg, %IDC_BTN_STOPALL ELSE CONTROL DISABLE g_hDlg, %IDC_BTN_STOPALL
END SUB

' ============================================================================
'  RESIZE LAYOUT
' ============================================================================

SUB ResizeControls()
    LOCAL rc AS RECT
    LOCAL cx AS LONG
    LOCAL cy AS LONG
    LOCAL hStatus AS DWORD
    LOCAL statusH AS LONG
    LOCAL btnH AS LONG
    LOCAL logH AS LONG
    LOCAL lvH AS LONG
    LOCAL y AS LONG
    LOCAL hDWP AS DWORD
    LOCAL hLV AS DWORD
    LOCAL hLog AS DWORD
    LOCAL hBtn1 AS DWORD
    LOCAL hBtn2 AS DWORD
    LOCAL hBtn3 AS DWORD
    LOCAL hBtn4 AS DWORD
    LOCAL dwFlags AS DWORD

    GetClientRect g_hDlg, rc
    cx = rc.nRight
    cy = rc.nBottom
    IF cx < 400 THEN EXIT SUB
    IF cy < 250 THEN EXIT SUB

    ' Get statusbar height
    CONTROL HANDLE g_hDlg, %IDC_STATUSBAR TO hStatus
    IF hStatus THEN
        GetWindowRect hStatus, rc
        statusH = rc.nBottom - rc.nTop
    ELSE
        statusH = 22
    END IF

    btnH = 26
    ' Proportional log height: 25% of available space, clamped 60..250
    logH = (cy - statusH - btnH - 12) \ 4
    IF logH < 60 THEN logH = 60
    IF logH > 250 THEN logH = 250

    ' ListView fills remaining space
    lvH = cy - statusH - btnH - logH - 12
    IF lvH < 60 THEN
        lvH = 60
        logH = cy - statusH - lvH - btnH - 12
        IF logH < 40 THEN logH = 40
    END IF

    ' Get all control handles for DeferWindowPos
    CONTROL HANDLE g_hDlg, %IDC_LISTVIEW    TO hLV
    CONTROL HANDLE g_hDlg, %IDC_LOG         TO hLog
    CONTROL HANDLE g_hDlg, %IDC_BTN_START   TO hBtn1
    CONTROL HANDLE g_hDlg, %IDC_BTN_STOP    TO hBtn2
    CONTROL HANDLE g_hDlg, %IDC_BTN_STOPALL TO hBtn3
    CONTROL HANDLE g_hDlg, %IDC_BTN_DUMP    TO hBtn4

    ' SWP_NOCOPYBITS: do NOT copy old pixels to new position.
    '   Without this flag, Windows BitBlts the old control content to
    '   the new position - the #1 source of "ghost" artifacts on buttons.
    '   This is the single most important flag for flicker-free resize.
    dwFlags = %SWP_NOZORDER OR %SWP_NOACTIVATE OR %SWP_NOCOPYBITS

    ' Use DeferWindowPos for atomic repositioning - prevents controls
    ' from overlapping each other during rapid window resizing.
    ' StatusBar is included in the batch so it moves atomically with
    ' all other controls, preventing its internal BitBlt from smearing
    ' old pixels over the button area.
    hDWP = BeginDeferWindowPos(7)
    IF hDWP THEN
        ' ListView: x=2, y=2
        hDWP = DeferWindowPos(hDWP, hLV, %NULL, 2, 2, cx - 4, lvH, dwFlags)

        ' Log: x=2, y=lvH+4
        y = lvH + 4
        hDWP = DeferWindowPos(hDWP, hLog, %NULL, 2, y, cx - 4, logH, dwFlags)

        ' Buttons: y=lvH+logH+8
        '   Start and Stop anchored to left edge,
        '   DUMP and Stop All anchored to right edge
        y = lvH + logH + 8
        hDWP = DeferWindowPos(hDWP, hBtn1, %NULL, 2, y, 90, btnH, dwFlags)
        hDWP = DeferWindowPos(hDWP, hBtn2, %NULL, 96, y, 90, btnH, dwFlags)
        hDWP = DeferWindowPos(hDWP, hBtn3, %NULL, cx - 92, y, 90, btnH, dwFlags)
        hDWP = DeferWindowPos(hDWP, hBtn4, %NULL, cx - 160, y, 60, btnH, dwFlags)

        ' StatusBar: position it at the bottom, full width, fixed height.
        ' Included in DeferWindowPos batch with SWP_NOCOPYBITS to prevent
        ' its internal BitBlt from smearing old pixels over buttons.
        y = cy - statusH
        hDWP = DeferWindowPos(hDWP, hStatus, %NULL, 0, y, cx, statusH, dwFlags)

        EndDeferWindowPos hDWP
    END IF
END SUB

' ============================================================================
'  NETWORK FUNCTIONS (using PowerBASIC built-in UDP)
' ============================================================================

' InitDiscovery - Open UDP socket on port 3950
FUNCTION InitDiscovery() AS LONG
    LOCAL fNum AS LONG

    fNum = FREEFILE
    ERRCLEAR

    UDP OPEN PORT 3950 AS #fNum TIMEOUT 60000

    IF ERR THEN
        FUNCTION = 0
        EXIT FUNCTION
    END IF

    g_fDiscFile = fNum
    g_fDiscOpen = 1

    ' Set up notification for async receive
    UDP NOTIFY #fNum, RECV TO g_hDlg AS %WM_UDP_DISC

    FUNCTION = 1
END FUNCTION

' DiscoveryProc - Process incoming EASSP INFO packets
SUB DiscoveryProc()
    LOCAL recvBuf AS STRING
    LOCAL fromIP AS DWORD
    LOCAL fromPort AS DWORD
    LOCAL tmpMac AS STRING
    LOCAL i AS LONG

    IF g_fDiscOpen = 0 THEN EXIT SUB

    ERRCLEAR
    UDP RECV #g_fDiscFile, FROM fromIP, fromPort, recvBuf

    IF ERR THEN EXIT SUB
    IF LEN(recvBuf) < %EASSP_HDR_SZ + %INFO_PAYLOAD_SZ THEN EXIT SUB

    ' Check magic bytes
    IF PEEK(BYTE, STRPTR(recvBuf)) <> %EASSP_MAGIC0 THEN EXIT SUB
    IF PEEK(BYTE, STRPTR(recvBuf) + 1) <> %EASSP_MAGIC1 THEN EXIT SUB
    IF PEEK(BYTE, STRPTR(recvBuf) + 2) <> %EASSP_VER THEN EXIT SUB
    IF PEEK(BYTE, STRPTR(recvBuf) + 3) <> %CMD_INFO THEN EXIT SUB

    ' Format MAC from bytes 17..22 (svc_info_payload: offset 9..14)
    tmpMac = ""
    FOR i = 17 TO 22
        tmpMac = tmpMac & RIGHT$("0" & HEX$(PEEK(BYTE, STRPTR(recvBuf) + i)), 2)
        IF i < 22 THEN tmpMac = tmpMac & ":"
    NEXT i

    ' Update device
    UpdateDevice tmpMac, fromIP, fromPort, STRPTR(recvBuf), LEN(recvBuf)
END SUB

' UpdateDevice - Find or create device, update info
SUB UpdateDevice(BYVAL sMac AS STRING, BYVAL dwIP AS DWORD, BYVAL dwPort AS DWORD, _
                 BYVAL pBuf AS DWORD, BYVAL bufLen AS LONG)
    LOCAL idx AS LONG
    LOCAL bNew AS LONG
    LOCAL tick AS DWORD
    LOCAL i AS LONG
    LOCAL b AS BYTE PTR

    b = pBuf

    EnterCriticalSection g_csDev

    ' Search for existing device
    idx = -1
    FOR i = 0 TO %MAX_DEVICES - 1
        IF g_Devs(i).dwActive THEN
            IF TRIM$(g_Devs(i).sMac) = TRIM$(sMac) THEN
                idx = i
                EXIT FOR
            END IF
        END IF
    NEXT i

    ' Find empty slot
    IF idx = -1 THEN
        FOR i = 0 TO %MAX_DEVICES - 1
            IF g_Devs(i).dwActive = 0 THEN
                idx = i
                bNew = 1
                EXIT FOR
            END IF
        NEXT i
    END IF

    IF idx = -1 THEN
        LeaveCriticalSection g_csDev
        EXIT SUB
    END IF

    tick = CLNG(TIMER * 1000)

    IF bNew THEN
        g_Devs(idx).dwActive = 1
        g_Devs(idx).sMac = sMac
        g_Devs(idx).dwDiscovered = tick
    END IF

    g_Devs(idx).dwIP         = dwIP
    g_Devs(idx).dwPort       = dwPort
    g_Devs(idx).dwStatus     = @b[8]
    g_Devs(idx).dwError      = @b[10]
    g_Devs(idx).dwChannels   = @b[11]      ' channels from INFO payload
    g_Devs(idx).dwCodec      = @b[9]       ' codec_id from INFO payload (5=ADPCM, 6=PCM)
    g_Devs(idx).dwSmpRate   = PEEK(DWORD, pBuf + 12)   ' offset 4 in payload
    ' bits_per_sample is at payload offset 32 = pBuf+40 (header 8 + payload 32)
    ' Only present in INFO v2 (33-byte payload). Guard against old 32-byte INFO.
    IF bufLen >= %EASSP_HDR_SZ + 33 THEN
        g_Devs(idx).dwBits = PEEK(BYTE, pBuf + 40)   ' bits_per_sample
    END IF
    g_Devs(idx).dwPktsSent   = PEEK(DWORD, pBuf + 23)   ' offset 15 in payload
    g_Devs(idx).dwFreeHeap   = PEEK(DWORD, pBuf + 27)   ' offset 19 in payload
    g_Devs(idx).dwLastSeen = tick

    ' wifi_rssi is int8_t at offset 31 - sign-extend to LONG
    LOCAL bRSSI AS LONG
    bRSSI = PEEK(BYTE, pBuf + 31)
    IF bRSSI > 127 THEN bRSSI = bRSSI - 256
    g_Devs(idx).dwRSSI = bRSSI

    ' Firmware (8 bytes at offset 32 in packet)
    g_Devs(idx).sFirmware = PEEK$(pBuf + 32, 8)

    LeaveCriticalSection g_csDev

    ' AddLog OUTSIDE CS - never hold CS during UI operations
    IF bNew THEN
        AddLog "New device: MAC=" & sMac & " IP=" & FormatIP(dwIP) & ":" & TRIM$(STR$(dwPort)) & _
               " rate=" & TRIM$(STR$(g_Devs(idx).dwSmpRate)) & " Hz"
    END IF
END SUB

' SendDiscover - Send DISCOVER command (heartbeat)
SUB SendDiscover(BYVAL targetIP AS DWORD, BYVAL targetPort AS DWORD)
    LOCAL sndBuf AS STRING
    LOCAL seq AS LONG
    LOCAL sndErr AS LONG

    sndBuf = CHR$(%EASSP_MAGIC0, %EASSP_MAGIC1, %EASSP_VER, %CMD_DISCOVER)
    InterlockedIncrement g_seqCnt
    seq = g_seqCnt
    sndBuf = sndBuf & CHR$(LO(BYTE, seq), HI(BYTE, seq), 0, 0)

    IF g_fDiscOpen THEN
        ERRCLEAR
        UDP SEND #g_fDiscFile, AT targetIP, targetPort, sndBuf
        sndErr = ERR
        IF sndErr THEN
            AddLog "[HB] SendDiscover FAILED err=" & TRIM$(STR$(sndErr)) & _
                   " to " & FormatIP(targetIP) & ":" & TRIM$(STR$(targetPort))
        END IF
    ELSE
        AddLog "[HB] SendDiscover SKIP - disc socket not open"
    END IF
END SUB

' SendConfigure - Send CONFIGURE command to device
'
' PROTOCOL PRINCIPLE: The receiver (this Windows program) NEVER dictates
' audio parameters to the ESP. CONFIGURE only tells the device WHERE to
' send audio (stream_port). The device is the audio authority - it streams
' exactly what is in its NVS config (set by AT+CH). The server learns the
' actual format from INFO packets and adapts its playback (WaveOut) to match.
' If the format is unsupported, the stream is rejected.
'
' The legacy `channels` field was removed from the payload - it was always
' ignored by the device anyway, but sending it violated the principle above.
SUB SendConfigure(BYVAL targetIP AS DWORD, BYVAL targetPort AS DWORD, _
                  BYVAL streamPort AS DWORD)
    LOCAL sndBuf AS STRING
    LOCAL nPort AS WORD
    LOCAL seq AS LONG

    sndBuf = CHR$(%EASSP_MAGIC0, %EASSP_MAGIC1, %EASSP_VER, %CMD_CONFIGURE)
    InterlockedIncrement g_seqCnt
    seq = g_seqCnt
    sndBuf = sndBuf & CHR$(LO(BYTE, seq), HI(BYTE, seq))
    sndBuf = sndBuf & CHR$(LO(BYTE, %CFG_PAYLOAD_SZ), HI(BYTE, %CFG_PAYLOAD_SZ))

    ' Stream port (2 bytes, network byte order) - the ONLY field in payload.
    ' NOTE: Avoid CINT() - it overflows for ports >= 32768
    nPort = htons(streamPort AND &HFFFF&)
    sndBuf = sndBuf & MKI$(nPort)

    IF g_fDiscOpen THEN
        ERRCLEAR
        UDP SEND #g_fDiscFile, AT targetIP, targetPort, sndBuf
    END IF
END SUB

' SendStop - Send STOP command to device (explicit immediate stop)
'
' Tells the device to stop streaming immediately. Unlike the old behavior
' (just stop sending DISCOVER heartbeats and wait for watchdog timeout),
' this gives an instant stop response.
'
' Packet format: [EASSP header 8 bytes, payload_len=0]
'   magic[2]   = EA 55
'   version    = 01
'   cmd        = 03 (CMD_STOP)
'   seq[2]     = sequence number
'   payload_len[2] = 00 00  (no payload)
SUB SendStop(BYVAL targetIP AS DWORD, BYVAL targetPort AS DWORD)
    LOCAL sndBuf AS STRING
    LOCAL seq AS LONG

    sndBuf = CHR$(%EASSP_MAGIC0, %EASSP_MAGIC1, %EASSP_VER, %CMD_STOP)
    InterlockedIncrement g_seqCnt
    seq = g_seqCnt
    sndBuf = sndBuf & CHR$(LO(BYTE, seq), HI(BYTE, seq), 0, 0)

    IF g_fDiscOpen THEN
        ERRCLEAR
        UDP SEND #g_fDiscFile, AT targetIP, targetPort, sndBuf
        IF ERR THEN
            AddLog "[STOP] SendStop FAILED err=" & TRIM$(STR$(ERR)) & _
                   " to " & FormatIP(targetIP) & ":" & TRIM$(STR$(targetPort))
        END IF
    ELSE
        AddLog "[STOP] SendStop SKIP - disc socket not open"
    END IF
END SUB

' ============================================================================
'  HEARTBEAT THREAD
' ============================================================================
THREAD FUNCTION HeartbeatThread(BYVAL param AS DWORD) AS DWORD
    LOCAL idx AS LONG
    LOCAL tick AS DWORD
    LOCAL elapsed AS DWORD
    LOCAL hbIP AS DWORD
    LOCAL hbPort AS DWORD
    LOCAL hbAudioPort AS DWORD
    LOCAL hbPktRecv AS DWORD
    LOCAL hbNeedResend AS LONG
    LOCAL hbResendIdx AS LONG
    LOCAL hbIter AS LONG          ' DIAG: iteration counter
    LOCAL hbSentThisIter AS LONG  ' DIAG: how many DISCOVER sent this iteration

    AddLog "[HB] HeartbeatThread started (interval=" & TRIM$(STR$(%HB_INTERVAL)) & "ms)"

    DO WHILE g_bRunning
        SLEEP %HB_INTERVAL
        IF g_bRunning = 0 THEN EXIT DO

        INCR hbIter
        hbSentThisIter = 0

        ' FIX H2: Collect (IP,Port) pairs under CS, then send OUTSIDE CS.
        ' Previously SendDiscover was called inside CS → UDP SEND could block
        ' → GUI thread (RefreshUI/UpdateButtonStates) waiting on CS → UI freeze.
        DIM hbTargets(0 TO %MAX_DEVICES - 1) AS LOCAL DWORD   ' IP
        DIM hbPorts(0 TO %MAX_DEVICES - 1) AS LOCAL DWORD      ' Port
        DIM hbCount AS LOCAL LONG
        hbCount = 0

        EnterCriticalSection g_csDev
        tick = CLNG(TIMER * 1000)
        hbNeedResend = 0
        hbResendIdx = -1

        FOR idx = 0 TO %MAX_DEVICES - 1
            IF g_Devs(idx).dwActive = 0 THEN ITERATE FOR

            IF g_Devs(idx).dwHBActive THEN
                ' Collect target for sending OUTSIDE CS
                hbTargets(hbCount) = g_Devs(idx).dwIP
                hbPorts(hbCount)   = g_Devs(idx).dwPort
                INCR hbCount

                ' AUTO-RESEND CONFIGURE: if we think we're streaming but ESP hasn't
                ' sent any audio packets in 3 seconds, the initial CONFIGURE was
                ' probably lost in WiFi. Resend it. ESP handles duplicate CONFIGURE
                ' safely (same dest = resets watchdog, different dest = stop+restart).
                hbPktRecv = g_Devs(idx).dwPktRecv
                IF hbPktRecv = 0 THEN
                    elapsed = tick - g_Devs(idx).dwStreamStart
                    IF elapsed > 3000 THEN
                        hbNeedResend = 1
                        hbResendIdx  = idx
                        hbIP        = g_Devs(idx).dwIP
                        hbPort      = g_Devs(idx).dwPort
                        hbAudioPort = g_Devs(idx).dwAudioPort
                    END IF
                END IF
            ELSE
                elapsed = tick - g_Devs(idx).dwLastSeen
                IF elapsed > %DEV_TIMEOUT_MS THEN
                    g_Devs(idx).dwActive = 0
                END IF
            END IF
        NEXT idx

        LeaveCriticalSection g_csDev

        ' Send DISCOVER heartbeats OUTSIDE CS (UDP SEND can block)
        FOR idx = 0 TO hbCount - 1
            SendDiscover hbTargets(idx), hbPorts(idx)
            INCR hbSentThisIter
        NEXT idx

        ' DIAG: log every heartbeat iteration when streaming, every 5th when idle
        IF hbSentThisIter > 0 THEN
            AddLog "[HB] iter=" & TRIM$(STR$(hbIter)) & _
                   " sent " & TRIM$(STR$(hbSentThisIter)) & " DISCOVER"
        ELSEIF (hbIter MOD 5) = 0 THEN
            AddLog "[HB] iter=" & TRIM$(STR$(hbIter)) & " idle (no HBActive devices)"
        END IF

        ' Resend CONFIGURE outside CS (UDP SEND can block)
        IF hbNeedResend AND hbResendIdx >= 0 THEN
            AddLog "No audio from device #" & TRIM$(STR$(hbResendIdx)) & _
                   " after 3s - resending CONFIGURE"
            SendConfigure hbIP, hbPort, hbAudioPort
        END IF
    LOOP

    FUNCTION = 0
END FUNCTION

' ============================================================================
'  AUDIO FUNCTIONS
' ============================================================================

' DecodeImaAdpcm - Decode IMA ADPCM data to 16-bit PCM
'
' BUGFIX: predictor update uses 32-bit arithmetic + clamp, NOT CINT(diff).
' CINT() silently truncates a LONG to a 16-bit INTEGER BEFORE the addition,
' which overflows at step_index >= 87 (step >= 29794, max delta = 55863..61438).
' Example: CINT(61438) = -4098 instead of 61438, so the decoder's predictor
' diverges from the encoder's on every loud transient, producing garbage
' until the next packet resets state from its DVI4 header. The ESP encoder
' uses 32-bit `predictor + delta` then clamp_int16(); this must mirror it.
SUB DecodeImaAdpcm(BYVAL pSrc AS BYTE PTR, BYVAL srcLen AS LONG, _
                   BYVAL pDst AS INTEGER PTR, predictor AS INTEGER, stepIndex AS LONG)
    LOCAL i AS LONG
    LOCAL b AS BYTE
    LOCAL CODE AS LONG
    LOCAL stp AS LONG
    LOCAL diff AS LONG
    LOCAL tmpPred AS LONG    ' 32-bit scratch for predictor + diff before clamp

    FOR i = 1 TO srcLen
        b = @pSrc
        INCR pSrc

        ' High nibble first
        CODE = (b AND &HF0) \ &H10
        GOSUB DoNibble
        @pDst = predictor
        INCR pDst

        ' Low nibble
        CODE = b AND &H0F
        GOSUB DoNibble
        @pDst = predictor
        INCR pDst
    NEXT i
    EXIT SUB

DoNibble:
    ' FIX C3: Clamp stepIndex BEFORE reading g_StepTable to prevent
    ' out-of-bounds read (stepIndex comes from raw packet, can be 0..255).
    IF stepIndex < 0 THEN stepIndex = 0
    IF stepIndex > 88 THEN stepIndex = 88
    stp = g_StepTable(stepIndex)
    diff = stp \ 8
    IF (CODE AND 4) THEN diff = diff + stp
    IF (CODE AND 2) THEN diff = diff + stp \ 2
    IF (CODE AND 1) THEN diff = diff + stp \ 4
    IF (CODE AND 8) THEN diff = -diff

    ' 32-bit add + clamp - mirrors the ESP encoder's clamp_int16(predictor + delta).
    ' Do NOT use CINT(diff): it truncates to 16-bit BEFORE the add and overflows
    ' at step_index >= 87 (max delta 55863..61438 > 32767), corrupting the
    ' predictor on loud transients.
    tmpPred = CLNG(predictor) + diff
    IF tmpPred < -32768 THEN tmpPred = -32768
    IF tmpPred > 32767 THEN tmpPred = 32767
    predictor = tmpPred

    stepIndex = stepIndex + g_IndexTable(CODE AND 7)
    IF stepIndex < 0 THEN stepIndex = 0
    IF stepIndex > 88 THEN stepIndex = 88
    RETURN
END SUB

' HzToRateEnum - Convert sample rate in Hz to EASSP enum (0..6).
' Returns 2 (16kHz) as fallback for unknown rates.
FUNCTION HzToRateEnum(BYVAL Hz AS DWORD) AS LONG
    SELECT CASE Hz
        CASE 8000:  FUNCTION = 0
        CASE 11025: FUNCTION = 1
        CASE 16000: FUNCTION = 2
        CASE 22050: FUNCTION = 3
        CASE 32000: FUNCTION = 4
        CASE 44100: FUNCTION = 5
        CASE 48000: FUNCTION = 6
        CASE ELSE:  FUNCTION = 2   ' fallback 16kHz
    END SELECT
END FUNCTION

' RateEnumToHz - Convert EASSP sample-rate enum (0..6) to Hz.
' Returns 16000 as fallback for out-of-range enums.
FUNCTION RateEnumToHz(BYVAL e AS LONG) AS DWORD
    SELECT CASE e
        CASE 0: FUNCTION = 8000
        CASE 1: FUNCTION = 11025
        CASE 2: FUNCTION = 16000
        CASE 3: FUNCTION = 22050
        CASE 4: FUNCTION = 32000
        CASE 5: FUNCTION = 44100
        CASE 6: FUNCTION = 48000
        CASE ELSE: FUNCTION = 16000
    END SELECT
END FUNCTION

' CodecName - Return human-readable codec name from codec ID.
FUNCTION CodecName(BYVAL codecId AS LONG) AS STRING
    SELECT CASE codecId
        CASE %CODEC_ID:     FUNCTION = "ADPCM"
        CASE %CODEC_ID_PCM: FUNCTION = "PCM"
        CASE ELSE:          FUNCTION = "Unknown(" & TRIM$(STR$(codecId)) & ")"
    END SELECT
END FUNCTION

' AudioThread - Per-device audio receive/decode/play thread
'   Supports mono (1 DVI4 block) and stereo (2 independent DVI4 blocks).
'   Stereo format: [pkt_header][DVI4_L][DVI4_R]
THREAD FUNCTION AudioThread(BYVAL param AS DWORD) AS DWORD
    LOCAL idx AS LONG
    LOCAL fNum AS LONG
    LOCAL waveOut AS DWORD
    LOCAL wIdx AS LONG
    LOCAL wIdx2 AS LONG
    LOCAL fromIP AS DWORD
    LOCAL fromPort AS DWORD
    LOCAL recvBuf AS STRING
    LOCAL adpcmLen AS LONG
    LOCAL pcmLen AS LONG
    LOCAL seqNum AS WORD
    LOCAL lastSeq AS WORD
    LOCAL pktRecv AS DWORD
    LOCAL predictor AS INTEGER
    LOCAL stepIndex AS LONG
    LOCAL predictor2 AS INTEGER
    LOCAL stepIndex2 AS LONG
    LOCAL bindPort AS LONG
    LOCAL lostPkt AS LONG
    LOCAL oooPkt AS LONG
    LOCAL nCh AS LONG
    LOCAL adpcmLenR AS LONG
    LOCAL adpcmLenL AS LONG
    LOCAL pcmLenL AS LONG
    LOCAL pcmLenR AS LONG
    LOCAL wfFmt AS WAVEFORMATEX

    idx = param

    ' Read audio port, channels, sample rate, bits, codec under CS
    EnterCriticalSection g_csDev
    bindPort = g_Devs(idx).dwAudioPort
    nCh = g_Devs(idx).dwChannels
    LOCAL devSmpRate AS DWORD
    devSmpRate = g_Devs(idx).dwSmpRate
    LOCAL devBits AS LONG
    devBits = g_Devs(idx).dwBits
    LOCAL devCodec AS LONG
    devCodec = g_Devs(idx).dwCodec
    ' curRateEnum tracks the sample-rate enum (0..6) currently open in WaveOut.
    ' Initialized from devSmpRate via Hz-to-enum conversion; updated on format change.
    LOCAL curRateEnum AS LONG
    curRateEnum = HzToRateEnum(devSmpRate)
    LeaveCriticalSection g_csDev

    IF nCh < 1 THEN nCh = 1
    IF nCh > 2 THEN nCh = 2
    IF devSmpRate < 8000 OR devSmpRate > 48000 THEN devSmpRate = 48000
    IF devBits <> 16 AND devBits <> 24 THEN devBits = 16

    ' Open UDP socket on the pre-assigned audio port
    fNum = FREEFILE
    ERRCLEAR
    UDP OPEN PORT bindPort AS #fNum TIMEOUT 60000
    IF ERR THEN
        AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
               ": UDP OPEN PORT " & TRIM$(STR$(bindPort)) & " FAILED err=" & TRIM$(STR$(ERR))
        EnterCriticalSection g_csDev
        ' NOTE: Do NOT clear hAudioThread here - main thread owns handle cleanup.
        ' Only clear fAudioFile and dwRunning so stop logic works correctly.
        g_Devs(idx).fAudioFile = 0
        g_Devs(idx).dwRunning  = 0
        LeaveCriticalSection g_csDev
        FUNCTION = 0
        EXIT FUNCTION
    END IF
    AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
           ": UDP OPEN PORT " & TRIM$(STR$(bindPort)) & " OK (file #" & TRIM$(STR$(fNum)) & ")"

    EnterCriticalSection g_csDev
    g_Devs(idx).fAudioFile = fNum
    LeaveCriticalSection g_csDev

    ' Allocate WAVEHDR array
    DIM whdrs(0 TO %NUM_WAVE_BUFS - 1) AS LOCAL WAVEHDR
    DIM pcmPtrs(0 TO %NUM_WAVE_BUFS - 1) AS LOCAL DWORD

       ' Allocate PCM buffers.
    ' Stereo interleave (in-place, no extra allocs) needs:
    '   [L 0..pcmLenL) [R pcmLenL..2*pcmLenL) [tempR 2*pcmLenL..3*pcmLenL) [tempL 3*pcmLenL..4*pcmLenL)
    '   Output [0..2*pcmLenL) overwrites L+R after temps are filled.
    ' With adpcmLenL clipped to WAVE_BUF_SZ/4 (=480), pcmLenL ? WAVE_BUF_SZ (=1920),
    ' Always allocate max buffer size (WAVE_BUF_SZ * 4) so that format changes
    ' (mono <-> stereo) on the fly don't require reallocation.
    LOCAL bufSz AS LONG
    bufSz = %WAVE_BUF_SZ * 4
    FOR wIdx2 = 0 TO %NUM_WAVE_BUFS - 1
        pcmPtrs(wIdx2) = HeapAlloc(g_hHeap, 0, bufSz)
        ' FIX C2: Check HeapAlloc return - NULL causes NULL-deref crash in
        ' waveOutPrepareHeader and decode loops.
        IF pcmPtrs(wIdx2) = 0 THEN
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                   ": HeapAlloc failed at buf " & TRIM$(STR$(wIdx2))
            ' Free previously allocated buffers
            LOCAL j AS LONG
            FOR j = 0 TO wIdx2 - 1
                IF pcmPtrs(j) THEN HeapFree g_hHeap, 0, pcmPtrs(j)
            NEXT j
            ERRCLEAR
            UDP CLOSE #fNum
            EnterCriticalSection g_csDev
            g_Devs(idx).fAudioFile = 0
            g_Devs(idx).dwRunning  = 0
            LeaveCriticalSection g_csDev
            FUNCTION = 0
            EXIT FUNCTION
        END IF
    NEXT wIdx2

    ' Init WAVEHDRs
    FOR wIdx2 = 0 TO %NUM_WAVE_BUFS - 1
        whdrs(wIdx2).lpData         = pcmPtrs(wIdx2)
        whdrs(wIdx2).dwBufferLength = bufSz
        whdrs(wIdx2).dwBytesRecorded = 0
        whdrs(wIdx2).dwUser         = 0
        whdrs(wIdx2).dwFlags        = 0
        whdrs(wIdx2).dwLoops        = 0
        whdrs(wIdx2).lpNext         = 0
        whdrs(wIdx2).Reserved       = 0
    NEXT wIdx2

    ' Set up WAVEFORMATEX - bits per sample depends on CODEC:
    '   ADPCM (codec=5): ALWAYS 16-bit. ESP dithers 24→16 before encoding,
    '     so the decoded PCM is 16-bit regardless of I2S bit depth.
    '   PCM (codec=6): uses devBits (16 or 24) from ESP config.
    '     24-bit PCM is passed through bit-perfect if the sound card supports it.
    '     If not, we fall back to 16-bit and convert on the fly.
    LOCAL outBits AS LONG
    IF devCodec = %CODEC_ID_PCM THEN
        outBits = devBits          ' PCM: use native bit depth
    ELSE
        outBits = 16               ' ADPCM: always 16-bit (dithered on ESP)
    END IF
    wfFmt.wFormatTag      = %WAVE_FORMAT_PCM
    wfFmt.nChannels       = nCh
    wfFmt.nSamplesPerSec  = devSmpRate
    wfFmt.wBitsPerSample  = outBits
    wfFmt.nBlockAlign     = nCh * (outBits \ 8)
    wfFmt.nAvgBytesPerSec = wfFmt.nSamplesPerSec * wfFmt.nBlockAlign
    wfFmt.cbSize          = 0

    ' Open WaveOut. Try 24-bit first if ESP is 24-bit; if the card rejects
    ' it (WAVERR_BADFORMAT or MMSYSERR), retry with 16-bit and mark for
    ' on-the-fly 24→16 conversion.
    LOCAL waveResult AS LONG
    waveResult = waveOutOpen(waveOut, %WAVE_MAPPER, wfFmt, 0, 0, %CALLBACK_NULL)
    IF waveResult <> 0 AND outBits = 24 THEN
        AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
               ": 24-bit WaveOut failed (err=" & TRIM$(STR$(waveResult)) & _
               "), falling back to 16-bit"
        outBits = 16
        wfFmt.wBitsPerSample  = 16
        wfFmt.nBlockAlign     = nCh * 2
        wfFmt.nAvgBytesPerSec = wfFmt.nSamplesPerSec * wfFmt.nBlockAlign
        waveResult = waveOutOpen(waveOut, %WAVE_MAPPER, wfFmt, 0, 0, %CALLBACK_NULL)
    END IF
    IF waveResult = 0 THEN
        LOCAL sCodecName AS STRING
        IF devCodec = %CODEC_ID_PCM THEN
            sCodecName = "PCM"
        ELSE
            sCodecName = "ADPCM"
        END IF
        IF outBits <> devBits AND devCodec = %CODEC_ID_PCM THEN
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                   ": WaveOut " & TRIM$(STR$(outBits)) & "-bit " & sCodecName & _
                   " (converting from " & TRIM$(STR$(devBits)) & "-bit)"
        ELSE
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                   ": WaveOut " & TRIM$(STR$(outBits)) & "-bit " & sCodecName & " (native)"
        END IF
    END IF
    IF waveResult <> 0 THEN
        AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
               ": waveOutOpen FAILED err=" & TRIM$(STR$(waveResult))
        FOR wIdx2 = 0 TO %NUM_WAVE_BUFS - 1
            IF pcmPtrs(wIdx2) THEN HeapFree g_hHeap, 0, pcmPtrs(wIdx2)
        NEXT wIdx2
        ERRCLEAR
        UDP CLOSE #fNum
        EnterCriticalSection g_csDev
        ' NOTE: Do NOT clear hAudioThread - main thread owns handle cleanup
        g_Devs(idx).dwRunning  = 0
        g_Devs(idx).fAudioFile = 0
        LeaveCriticalSection g_csDev
        FUNCTION = 0
        EXIT FUNCTION
    END IF

    EnterCriticalSection g_csDev
    g_Devs(idx).hWaveOut = waveOut
    LeaveCriticalSection g_csDev

    ' Prepare headers
    FOR wIdx2 = 0 TO %NUM_WAVE_BUFS - 1
        waveOutPrepareHeader waveOut, whdrs(wIdx2), SIZEOF(WAVEHDR)
    NEXT wIdx2

    ' Init stats under CS
    EnterCriticalSection g_csDev
    g_Devs(idx).dwRunning     = 1
    g_Devs(idx).dwPktRecv     = 0
    g_Devs(idx).dwPktOOO      = 0
    g_Devs(idx).dwPktLost     = 0
    g_Devs(idx).dwBytesRecv   = 0
    g_Devs(idx).wLastSeq      = 0
    g_Devs(idx).dwStreamStart = CLNG(TIMER * 1000)
    LeaveCriticalSection g_csDev

    predictor  = 0
    stepIndex  = 0
    predictor2 = 0
    stepIndex2 = 0
    lastSeq = 0
    pktRecv = 0
    wIdx = 0

    ' ---- Main audio loop ----
    LOCAL audRecvCount AS LONG    ' DIAG: count of successful RECV
    LOCAL audErrCount  AS LONG    ' DIAG: count of ERR on RECV
    audRecvCount = 0
    audErrCount  = 0
    DO WHILE g_Devs(idx).dwRunning
        ERRCLEAR
        UDP RECV #fNum, FROM fromIP, fromPort, recvBuf

        IF ERR THEN
            INCR audErrCount
            ' DIAG: log first ERR and every 5000th (≈10s of idle loops)
            IF audErrCount = 1 OR (audErrCount MOD 5000) = 0 THEN
                AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                       ": UDP RECV err=" & TRIM$(STR$(ERR)) & _
                       " (count=" & TRIM$(STR$(audErrCount)) & ") - no data?"
            END IF
            SLEEP 2
            ITERATE DO
        END IF

        IF LEN(recvBuf) < %PKT_HDR_SZ THEN ITERATE DO

        ' DIAG: log first received packet
        INCR audRecvCount
        IF audRecvCount = 1 THEN
            LOCAL b0 AS LONG, b1 AS LONG, b2 AS LONG, b3 AS LONG
            LOCAL b4 AS LONG, b5 AS LONG, b6 AS LONG, b7 AS LONG, b8 AS LONG
            b0 = PEEK(BYTE, STRPTR(recvBuf) + 0)
            b1 = PEEK(BYTE, STRPTR(recvBuf) + 1)
            b2 = PEEK(BYTE, STRPTR(recvBuf) + 2)
            b3 = PEEK(BYTE, STRPTR(recvBuf) + 3)
            b4 = PEEK(BYTE, STRPTR(recvBuf) + 4)
            b5 = PEEK(BYTE, STRPTR(recvBuf) + 5)
            b6 = PEEK(BYTE, STRPTR(recvBuf) + 6)
            b7 = PEEK(BYTE, STRPTR(recvBuf) + 7)
            b8 = PEEK(BYTE, STRPTR(recvBuf) + 8)
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                   ": FIRST pkt len=" & TRIM$(STR$(LEN(recvBuf))) & _
                   " from " & FormatIP(fromIP) & ":" & TRIM$(STR$(fromPort))
            AddLog "[AUD]   bytes[0..8]= " & TRIM$(STR$(b0)) & " " & TRIM$(STR$(b1)) & _
                   " " & TRIM$(STR$(b2)) & " " & TRIM$(STR$(b3)) & _
                   " " & TRIM$(STR$(b4)) & " " & TRIM$(STR$(b5)) & _
                   " " & TRIM$(STR$(b6)) & " " & TRIM$(STR$(b7)) & _
                   " " & TRIM$(STR$(b8))
            AddLog "[AUD]   byte[6]=codec=" & TRIM$(STR$(b6)) & _
                   " (expect " & TRIM$(STR$(%CODEC_ID)) & ")  byte[8]=ch=" & _
                   TRIM$(STR$(b8)) & " (expect nCh=" & TRIM$(STR$(nCh)) & ")"
        END IF

        ' Read format fields from packet header (offsets 6,7,8,14).
        '   offset 6:  codec (5=ADPCM, 6=PCM)
        '   offset 7:  sample_rate_enum (0..6 → 8k..48k)
        '   offset 8:  channels (1 or 2)
        '   offset 14: bits (uint16 LE, 16 or 24) - I2S bit depth
        LOCAL pktCodec AS LONG
        LOCAL pktRateEnum AS LONG
        LOCAL pktCh AS LONG
        LOCAL pktBits AS LONG
        pktCodec = PEEK(BYTE, STRPTR(recvBuf) + 6)
        pktRateEnum = PEEK(BYTE, STRPTR(recvBuf) + 7)
        pktCh = PEEK(BYTE, STRPTR(recvBuf) + 8)
        pktBits = PEEK(WORD, STRPTR(recvBuf) + 14)

        ' Validate codec is known (reject garbage, but allow format change)
        IF pktCodec <> %CODEC_ID AND pktCodec <> %CODEC_ID_PCM THEN
            IF audRecvCount <= 3 THEN
                AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                       ": REJECT pkt#" & TRIM$(STR$(audRecvCount)) & _
                       " codec=" & TRIM$(STR$(pktCodec)) & " (expected 5 or 6)"
            END IF
            ITERATE DO
        END IF

        ' Clamp bits to valid range
        IF pktBits <> 16 AND pktBits <> 24 THEN pktBits = 16
        IF pktCh < 1 THEN pktCh = 1
        IF pktCh > 2 THEN pktCh = 2

        ' ---- WAV dump mode ----
        ' Write decoded PCM audio to a WAV file.
        ' For PCM codec: raw payload is already PCM, write directly.
        ' For ADPCM codec: decoded PCM is in pcmPtrs after decode (handled below).
        ' First packet: determine format and open WAV file with header.
        IF g_bDumping THEN
            IF g_dumpWavReady = 0 THEN
                ' First packet - set format and open file
                g_dumpCodec = pktCodec
                g_dumpSampleRate = RateEnumToHz(pktRateEnum)
                g_dumpChannels = pktCh
                IF pktCodec = %CODEC_ID_PCM THEN
                    g_dumpBits = pktBits
                ELSE
                    g_dumpBits = 16  ' ADPCM decodes to 16-bit
                END IF
                g_dumpFileIdx = 1
                EnterCriticalSection g_csDump
                OpenNewDumpFile
                LeaveCriticalSection g_csDump
            END IF

            ' For PCM codec: write raw payload directly (it's already PCM)
            IF pktCodec = %CODEC_ID_PCM AND g_hDumpFile THEN
                LOCAL dumpLen AS LONG
                dumpLen = LEN(recvBuf) - %PKT_HDR_SZ
                IF dumpLen > 0 THEN
                    EnterCriticalSection g_csDump
                    IF g_hDumpFile THEN
                        PUT$ #g_hDumpFile, PEEK$(STRPTR(recvBuf) + %PKT_HDR_SZ, dumpLen)
                        g_dumpDataSize = g_dumpDataSize + dumpLen
                        ' Auto-split at 1 GB
                        IF g_dumpDataSize > 1073741780 THEN
                            UpdateWavHeader
                            CLOSE g_hDumpFile
                            g_hDumpFile = 0
                            INCR g_dumpFileIdx
                            OpenNewDumpFile
                        END IF
                    END IF
                    LeaveCriticalSection g_csDump
                END IF
            END IF
            ' For ADPCM: decoded PCM is written after decode (see below)
        END IF

        ' ---- Format change detection ----
        ' Compare packet format with the format currently open in WaveOut.
        ' If any field changed (codec, channels, sample rate, bits), close
        ' WaveOut and reopen with the new format. This allows on-the-fly
        ' format changes via AT+HOTRESTART on the ESP side.
        IF pktCodec <> devCodec OR pktCh <> nCh OR _
           pktRateEnum <> curRateEnum OR pktBits <> devBits THEN
            LOCAL sOldFmt AS STRING, sNewFmt AS STRING
            sOldFmt = CodecName(devCodec) & " " & TRIM$(STR$(devSmpRate)) & "Hz " & _
                      TRIM$(STR$(nCh)) & "ch " & TRIM$(STR$(devBits)) & "-bit"
            sNewFmt = CodecName(pktCodec) & " " & TRIM$(STR$(RateEnumToHz(pktRateEnum))) & "Hz " & _
                      TRIM$(STR$(pktCh)) & "ch " & TRIM$(STR$(pktBits)) & "-bit"
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                   ": format changed (" & sOldFmt & " -> " & sNewFmt & ") - reopening WaveOut"

            ' Close old WaveOut
            IF waveOut THEN
                waveOutReset waveOut
                FOR wIdx2 = 0 TO %NUM_WAVE_BUFS - 1
                    waveOutUnprepareHeader waveOut, whdrs(wIdx2), SIZEOF(WAVEHDR)
                NEXT wIdx2
                waveOutClose waveOut
                waveOut = 0
            END IF

            ' Update local format variables from packet
            devCodec = pktCodec
            nCh = pktCh
            curRateEnum = pktRateEnum
            devBits = pktBits
            devSmpRate = RateEnumToHz(curRateEnum)

            ' Update g_Devs so UI shows the new format
            EnterCriticalSection g_csDev
            g_Devs(idx).dwCodec = devCodec
            g_Devs(idx).dwChannels = nCh
            g_Devs(idx).dwSmpRate = devSmpRate
            g_Devs(idx).dwBits = devBits
            LeaveCriticalSection g_csDev

            ' Jump to WaveOut reopen (resets decoder, sets up WAVEFORMATEX,
            ' opens WaveOut, prepares headers)
            GOTO reopen_waveout
        END IF

        ' ---- Raw PCM passthrough (codec == 6) ----
        ' ESP sends raw PCM without compression.
        '   16-bit: payload is int16 samples - copy directly (bit-perfect).
        '   24-bit: payload is 3 bytes/sample (LE, sign-extended 24-bit).
        '     If WaveOut is 24-bit (sound card supports it): copy directly.
        '     If WaveOut is 16-bit (fallback): convert 24→16 by taking
        '       mid+hi bytes (>>8), losing the low 8 bits.
        IF pktCodec = %CODEC_ID_PCM THEN
            IF devBits = 24 AND outBits = 16 THEN
                ' 24-bit ESP → 16-bit WaveOut: downsample by >>8.
                ' Take bytes [1,2] of each 3-byte sample (mid+hi).
                LOCAL pcm24Len AS LONG
                LOCAL pcm24Src AS BYTE PTR
                LOCAL pcm16Dst AS BYTE PTR
                LOCAL si24 AS LONG
                pcm24Len = LEN(recvBuf) - %PKT_HDR_SZ
                pcm24Src = STRPTR(recvBuf) + %PKT_HDR_SZ
                pcm16Dst = pcmPtrs(wIdx)
                LOCAL outIdx AS LONG
                outIdx = 0
                FOR si24 = 0 TO pcm24Len - 3 STEP 3
                    @pcm16Dst[outIdx] = @pcm24Src[si24 + 1]   ' mid byte → lo
                    INCR outIdx
                    @pcm16Dst[outIdx] = @pcm24Src[si24 + 2]   ' hi byte → hi
                    INCR outIdx
                    IF outIdx >= bufSz - 1 THEN EXIT FOR
                NEXT si24
                pcmLen = outIdx
            ELSE
                ' Bit-perfect passthrough (16→16 or 24→24): copy directly.
                pcmLen = LEN(recvBuf) - %PKT_HDR_SZ
                IF pcmLen > bufSz THEN pcmLen = bufSz
                IF pcmLen > 0 THEN
                    POKE$ pcmPtrs(wIdx), PEEK$(STRPTR(recvBuf) + %PKT_HDR_SZ, pcmLen)
                END IF
            END IF
            GOTO pcm_ready
        END IF

        ' From here on: ADPCM decoding path (codec == 5)
        IF LEN(recvBuf) < %PKT_HDR_SZ + %DVI4_HDR_SZ THEN ITERATE DO

        IF nCh = 1 THEN
            ' ---- MONO: single DVI4 block ----
            adpcmLen = LEN(recvBuf) - %PKT_HDR_SZ - %DVI4_HDR_SZ
            IF adpcmLen <= 0 THEN ITERATE DO

            ' Clip adpcmLen to what the PCM buffer can hold.
            ' DecodeImaAdpcm writes srcLen*4 bytes (2 samples x 2 bytes per
            ' input byte); the buffer is WAVE_BUF_SZ bytes. Without this
            ' clip, large frames (e.g. 48kHz/40ms -> adpcmLen=960 -> 3840
            ' bytes) overflow the 1920-byte heap buffer.
            IF adpcmLen > (%WAVE_BUF_SZ \ 4) THEN adpcmLen = %WAVE_BUF_SZ \ 4

            predictor = CVI(PEEK$(STRPTR(recvBuf) + 16, 2))
            stepIndex = PEEK(BYTE, STRPTR(recvBuf) + 18)

            pcmLen = adpcmLen * 4
            IF pcmLen > %WAVE_BUF_SZ THEN pcmLen = %WAVE_BUF_SZ

            DecodeImaAdpcm STRPTR(recvBuf) + 20, adpcmLen, pcmPtrs(wIdx), predictor, stepIndex

            ' === DIAGNOSTIC LOG: first 5 packets ===
            ' Shows what the decoder actually receives and produces.
            ' Remove this block after debugging is complete.
            IF pktRecv < 5 THEN
                LOCAL pPcm AS INTEGER PTR
                LOCAL pkPeak AS LONG
                LOCAL pkI AS LONG
                pPcm = pcmPtrs(wIdx)
                pkPeak = 0
                FOR pkI = 0 TO (pcmLen \ 2) - 1
                    IF ABS(@pPcm[pkI]) > pkPeak THEN pkPeak = ABS(@pPcm[pkI])
                NEXT pkI
                AddLog "Pkt" & TRIM$(STR$(pktRecv)) & ": pktLen=" & TRIM$(STR$(LEN(recvBuf))) & _
                       " adpcmLen=" & TRIM$(STR$(adpcmLen)) & _
                       " pcmLen=" & TRIM$(STR$(pcmLen)) & _
                       " pred=" & TRIM$(STR$(predictor)) & _
                       " idx=" & TRIM$(STR$(stepIndex)) & _
                       " pcm[0]=" & TRIM$(STR$(@pPcm[0])) & _
                       " pcm[1]=" & TRIM$(STR$(@pPcm[1])) & _
                       " peak=" & TRIM$(STR$(pkPeak))
            END IF
            ' === END DIAGNOSTIC LOG ===

        ELSE
            ' ---- STEREO: two independent DVI4 blocks ----
            ' [pkt_header 16] [dvi4_hdr_L 4] [adpcm_L] [dvi4_hdr_R 4] [adpcm_R]
            ' BUGFIX: subtract BOTH DVI4 headers, not just one.
            adpcmLenL = (LEN(recvBuf) - %PKT_HDR_SZ - 2 * %DVI4_HDR_SZ) \ 2
            IF adpcmLenL <= 0 THEN ITERATE DO

            ' Clip each channel's ADPCM length to WAVE_BUF_SZ/4 bytes so the
            ' decoded PCM (adpcmLenL*4) fits in the per-channel PCM region
            ' (WAVE_BUF_SZ bytes). Prevents heap overflow on large frames.
            IF adpcmLenL > (%WAVE_BUF_SZ \ 4) THEN adpcmLenL = %WAVE_BUF_SZ \ 4

            ' Decode left channel
            predictor = CVI(PEEK$(STRPTR(recvBuf) + 16, 2))
            stepIndex = PEEK(BYTE, STRPTR(recvBuf) + 18)
            DecodeImaAdpcm STRPTR(recvBuf) + 20, adpcmLenL, pcmPtrs(wIdx), predictor, stepIndex
            pcmLenL = adpcmLenL * 4

            ' Decode right channel
            LOCAL offsetR AS LONG
            offsetR = %PKT_HDR_SZ + %DVI4_HDR_SZ + adpcmLenL
            IF offsetR + %DVI4_HDR_SZ > LEN(recvBuf) THEN ITERATE DO

            predictor2 = CVI(PEEK$(STRPTR(recvBuf) + offsetR, 2))
            stepIndex2 = PEEK(BYTE, STRPTR(recvBuf) + offsetR + 2)
            DecodeImaAdpcm STRPTR(recvBuf) + offsetR + 4, adpcmLenL, pcmPtrs(wIdx) + pcmLenL, predictor2, stepIndex2
            pcmLenR = adpcmLenL * 4

            ' Interleave L,R into the PCM buffer
            ' Left is at pcmPtrs(wIdx), Right is at pcmPtrs(wIdx) + pcmLenL
            ' We need to interleave: L0,R0,L1,R1...
            ' BUGFIX: pL and pOut point to the SAME buffer (pcmPtrs).
            ' Writing R0 to pOut[1] overwrites pL[1] before it's read!
            ' Fix: Copy BOTH channels to temp areas before interleaving.
            ' Buffer layout: [L 640B] [R 640B] [tempR 640B] [tempL 640B] [unused]
            ' Total: 2560B out of 3840B available.
            LOCAL pL AS INTEGER PTR
            LOCAL pR AS INTEGER PTR
            LOCAL pOut AS INTEGER PTR
            LOCAL nSamp AS LONG
            LOCAL si AS LONG
            LOCAL pTempL AS INTEGER PTR
            LOCAL pTempR AS INTEGER PTR
            pL = pcmPtrs(wIdx)
            pR = pcmPtrs(wIdx) + pcmLenL
            nSamp = pcmLenL \ 2
                      ' Copy both channels to safe temp areas beyond the decoded region.
            ' pTempR at byte offset 2*pcmLenL, pTempL at byte offset 3*pcmLenL
            ' (offsets scale with pcmLenL; only safe because bufSz = 4*WAVE_BUF_SZ for stereo)
            pTempR = pcmPtrs(wIdx) + (2 * pcmLenL)
            pTempL = pcmPtrs(wIdx) + (3 * pcmLenL)


            FOR si = 0 TO nSamp - 1
                @pTempR[si] = @pR[si]
                @pTempL[si] = @pL[si]
            NEXT si
            ' Interleave from temp buffers (safe, no overwriting)
            pOut = pcmPtrs(wIdx)
            FOR si = 0 TO nSamp - 1
                @pOut = @pTempL[si]
                INCR pOut
                @pOut = @pTempR[si]
                INCR pOut
            NEXT si

            pcmLen = pcmLenL + pcmLenR
            ' Bounds check for stereo - clip to allocated buffer size
            IF pcmLen > bufSz THEN pcmLen = bufSz
        END IF

pcm_ready:
        INCR pktRecv
        IF pktRecv = 1 THEN
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                   ": FIRST pkt ACCEPTED (pktRecv=1) - audio pipeline active"
        END IF

        ' Calculate packet loss locally
        seqNum = CVWRD(PEEK$(STRPTR(recvBuf), 2))
        lostPkt = 0
        oooPkt  = 0
        IF pktRecv > 1 THEN
            ' Modular 16-bit sequence arithmetic to correctly handle the
            ' 65535->0 wrap. The old code compared seqNum <> lastSeq+1 where
            ' lastSeq+1 promoted to LONG (65536) and misclassified the wrap
            ' as out-of-order. Now: diff = wrapped gap; 0 = in order,
            ' 1..32767 = lost packet count, >=32768 = out-of-order/dup.
            LOCAL seqDiff AS LONG
            seqDiff = (CLNG(seqNum) - CLNG(lastSeq) - 1) AND &HFFFF&
            IF seqDiff > 0 THEN
                IF seqDiff < 32768 THEN
                    lostPkt = seqDiff
                ELSE
                    oooPkt = 1
                END IF
            END IF
        END IF
        lastSeq = seqNum

        ' Update stats under CS
        EnterCriticalSection g_csDev
        g_Devs(idx).dwPktRecv   = pktRecv
        g_Devs(idx).wLastSeq    = lastSeq
        g_Devs(idx).dwBytesRecv = g_Devs(idx).dwBytesRecv + pcmLen
        IF oooPkt  THEN g_Devs(idx).dwPktOOO  = g_Devs(idx).dwPktOOO  + oooPkt
        IF lostPkt THEN g_Devs(idx).dwPktLost = g_Devs(idx).dwPktLost + lostPkt
        LeaveCriticalSection g_csDev

        ' ---- WAV dump: ADPCM decoded PCM ----
        ' For ADPCM codec: write decoded 16-bit PCM from pcmPtrs to WAV file.
        ' (PCM codec is already handled above with raw payload write.)
        IF g_bDumping AND pktCodec = %CODEC_ID AND g_hDumpFile AND pcmLen > 0 THEN
            EnterCriticalSection g_csDump
            IF g_hDumpFile THEN
                PUT$ #g_hDumpFile, PEEK$(pcmPtrs(wIdx), pcmLen)
                g_dumpDataSize = g_dumpDataSize + pcmLen
                ' Auto-split at 1 GB
                IF g_dumpDataSize > 1073741780 THEN
                    UpdateWavHeader
                    CLOSE g_hDumpFile
                    g_hDumpFile = 0
                    INCR g_dumpFileIdx
                    OpenNewDumpFile
                END IF
            END IF
            LeaveCriticalSection g_csDump
        END IF

        ' FIX M1+M2: Guard against waveOut=NULL (failed reopen) - skip
        ' waveOutWrite if no valid WaveOut handle.
        IF waveOut <> 0 AND (whdrs(wIdx).dwFlags AND %WHDR_INQUEUE) = 0 THEN
            whdrs(wIdx).dwBufferLength = pcmLen
            whdrs(wIdx).dwBytesRecorded = pcmLen
            waveOutWrite waveOut, whdrs(wIdx), SIZEOF(WAVEHDR)
        END IF

        wIdx = (wIdx + 1) MOD %NUM_WAVE_BUFS
        ITERATE DO

reopen_waveout:
        ' ---- WaveOut reopen after format change ----
        ' Reached via GOTO from the format-change detection block above.
        ' At this point: old WaveOut is closed, devCodec/nCh/devSmpRate/devBits
        ' are updated from the packet. We need to:
        '   1. Reset ADPCM decoder state (new stream starts fresh)
        '   2. Set up WAVEFORMATEX with new format
        '   3. Open WaveOut (with 24->16 fallback)
        '   4. Prepare headers
        '   5. Continue the main loop

        ' Reset ADPCM decoder state - new stream may have different predictor
        predictor  = 0
        stepIndex  = 0
        predictor2 = 0
        stepIndex2 = 0
        lastSeq = 0

        ' Set up WAVEFORMATEX - bits per sample depends on CODEC:
        '   ADPCM (codec=5): ALWAYS 16-bit (ESP dithers 24->16 before encoding)
        '   PCM (codec=6): uses devBits (16 or 24) from packet
        IF devCodec = %CODEC_ID_PCM THEN
            outBits = devBits
        ELSE
            outBits = 16
        END IF
        wfFmt.wFormatTag      = %WAVE_FORMAT_PCM
        wfFmt.nChannels       = nCh
        wfFmt.nSamplesPerSec  = devSmpRate
        wfFmt.wBitsPerSample  = outBits
        wfFmt.nBlockAlign     = nCh * (outBits \ 8)
        wfFmt.nAvgBytesPerSec = wfFmt.nSamplesPerSec * wfFmt.nBlockAlign
        wfFmt.cbSize          = 0

        ' Open WaveOut. Try 24-bit first if PCM 24-bit; if the card rejects
        ' it, retry with 16-bit and mark for on-the-fly 24->16 conversion.
        waveResult = waveOutOpen(waveOut, %WAVE_MAPPER, wfFmt, 0, 0, %CALLBACK_NULL)
        IF waveResult <> 0 AND outBits = 24 THEN
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                   ": 24-bit WaveOut failed on reopen (err=" & TRIM$(STR$(waveResult)) & _
                   "), falling back to 16-bit"
            outBits = 16
            wfFmt.wBitsPerSample  = 16
            wfFmt.nBlockAlign     = nCh * 2
            wfFmt.nAvgBytesPerSec = wfFmt.nSamplesPerSec * wfFmt.nBlockAlign
            waveResult = waveOutOpen(waveOut, %WAVE_MAPPER, wfFmt, 0, 0, %CALLBACK_NULL)
        END IF

        IF waveResult = 0 THEN
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                   ": WaveOut reopened " & TRIM$(STR$(outBits)) & "-bit " & _
                   CodecName(devCodec) & " " & TRIM$(STR$(devSmpRate)) & "Hz " & _
                   TRIM$(STR$(nCh)) & "ch"

            ' Store new waveOut handle
            EnterCriticalSection g_csDev
            g_Devs(idx).hWaveOut = waveOut
            LeaveCriticalSection g_csDev

            ' Prepare headers for the new WaveOut
            FOR wIdx2 = 0 TO %NUM_WAVE_BUFS - 1
                whdrs(wIdx2).dwFlags = 0
                whdrs(wIdx2).dwBufferLength = bufSz
                waveOutPrepareHeader waveOut, whdrs(wIdx2), SIZEOF(WAVEHDR)
            NEXT wIdx2
        ELSE
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                   ": waveOutOpen FAILED on reopen err=" & TRIM$(STR$(waveResult)) & _
                   " - audio disabled until next format change"
            ' Keep receiving packets (stream stays active) but no playback.
            ' Next format change will try again.
        END IF

        ITERATE DO
    LOOP

    ' ---- Cleanup ----
    IF waveOut THEN
        waveOutReset waveOut
        FOR wIdx2 = 0 TO %NUM_WAVE_BUFS - 1
            waveOutUnprepareHeader waveOut, whdrs(wIdx2), SIZEOF(WAVEHDR)
        NEXT wIdx2
        waveOutClose waveOut
    END IF

    ' Finalize WAV dump if still active
    EnterCriticalSection g_csDump
    IF g_hDumpFile THEN
        UpdateWavHeader
        CLOSE g_hDumpFile
        g_hDumpFile = 0
    END IF
    LeaveCriticalSection g_csDump

    ' UDP may have been closed from outside (StopAllStreams) on shutdown,
    ' so use ERRCLEAR to handle already-closed file gracefully
    ERRCLEAR
    UDP CLOSE #fNum
    ERRCLEAR

    FOR wIdx2 = 0 TO %NUM_WAVE_BUFS - 1
        IF pcmPtrs(wIdx2) THEN HeapFree g_hHeap, 0, pcmPtrs(wIdx2)
    NEXT wIdx2

    ' Clear state under CS so other threads see consistent state.
    ' NOTE: hAudioThread is NOT cleared here - the main thread is responsible
    ' for reading and closing the thread handle (StopCheckedStreams/WaitForAllThreads).
    ' Clearing it here would cause a handle leak if the main thread hasn't
    ' read it yet.
    EnterCriticalSection g_csDev
    g_Devs(idx).fAudioFile = 0
    g_Devs(idx).dwRunning  = 0
    LeaveCriticalSection g_csDev

    FUNCTION = 0
END FUNCTION

' ============================================================================
'  STREAM MANAGEMENT
' ============================================================================

' StartCheckedStreams - Start audio stream for all checked devices
SUB StartCheckedStreams()
    LOCAL hLV AS DWORD
    LOCAL i AS LONG
    LOCAL nCount AS LONG
    LOCAL devIdx AS LONG
    LOCAL lvi AS LV_ITM
    LOCAL started AS LONG
    LOCAL saveIP AS DWORD
    LOCAL savePort AS DWORD
    LOCAL saveMac AS STRING
    LOCAL saveAudioPort AS DWORD
    LOCAL hThread AS DWORD

    CONTROL HANDLE g_hDlg, %IDC_LISTVIEW TO hLV
    IF hLV = 0 THEN EXIT SUB

    nCount = SendMessage(hLV, %LVM_GETITEMCOUNT, 0, 0)

    FOR i = 0 TO nCount - 1
        IF IsItemChecked(hLV, i) = 0 THEN ITERATE FOR

        lvi.mask     = %LVIF_PARAM
        lvi.iItem    = i
        lvi.iSubItem = 0
        SendMessage hLV, %LVM_GETITEM, 0, VARPTR(lvi)
        devIdx = lvi.lParam

        IF devIdx < 0 OR devIdx >= %MAX_DEVICES THEN ITERATE FOR

        EnterCriticalSection g_csDev

        ' Skip if not active or already streaming
        IF g_Devs(devIdx).dwActive = 0 OR g_Devs(devIdx).dwHBActive <> 0 THEN
            LeaveCriticalSection g_csDev
            ITERATE FOR
        END IF

        g_Devs(devIdx).dwStatus    = %STS_STREAM
        g_Devs(devIdx).dwError     = 0
        g_Devs(devIdx).dwAudioPort = %AUDIO_PORT_BASE + devIdx

        ' Save values needed outside CS
        saveIP        = g_Devs(devIdx).dwIP
        savePort      = g_Devs(devIdx).dwPort
        saveMac       = TRIM$(g_Devs(devIdx).sMac)
        saveAudioPort = g_Devs(devIdx).dwAudioPort

        LeaveCriticalSection g_csDev

        ' Create thread outside CS
        THREAD CREATE AudioThread(devIdx) TO hThread

        ' Store thread handle + mark streaming under CS
        ' (dwHBActive set AFTER thread creation so stop logic can find hAudioThread)
        EnterCriticalSection g_csDev
        g_Devs(devIdx).hAudioThread = hThread
        g_Devs(devIdx).dwHBActive   = 1
        LeaveCriticalSection g_csDev

        AddLog "[HB] dwHBActive=1 for dev #" & TRIM$(STR$(devIdx)) & _
               " - heartbeat will be sent every " & TRIM$(STR$(%HB_INTERVAL)) & "ms"

        SLEEP 50

        ' Send CONFIGURE with RETRIES - UDP is unreliable, single send can be lost.
        ' Send 3 times with 200ms gaps. ESP deduplicates (same dest = resets watchdog),
        ' so duplicates are harmless. This fixes "Start doesn't work" when first
        ' CONFIGURE packet is lost in WiFi.
        '
        ' PROTOCOL PRINCIPLE: The receiver (this Windows program) NEVER dictates
        ' audio parameters to the ESP. CONFIGURE only tells ESP WHERE to send
        ' audio (stream_port). Channels=0 means "use your NVS config" - ESP is
        ' the audio authority, the server adapts to whatever ESP sends.
        ' The server learns the actual format from INFO packets and opens
        ' WaveOut to match. If the format is unsupported, the stream is rejected.
        LOCAL cfgRetry AS LONG
        FOR cfgRetry = 1 TO 3
            SendConfigure saveIP, savePort, saveAudioPort  ' CONFIGURE = just "send audio to this port"
            IF cfgRetry < 3 THEN SLEEP 200
        NEXT cfgRetry

        INCR started
        AddLog "Stream started: " & saveMac & " -> " & _
               FormatIP(saveIP) & ":" & TRIM$(STR$(savePort)) & _
               " port=" & TRIM$(STR$(saveAudioPort)) & _
               " rate=" & TRIM$(STR$(g_Devs(devIdx).dwSmpRate)) & " Hz"
    NEXT i

    IF started = 0 THEN
        AddLog "No devices to start (not checked or already streaming)"
    ELSE
        AddLog "Started" & STR$(started) & " stream(s)"
    END IF
END SUB

' StopCheckedStreams - Stop audio stream for all checked streaming devices
SUB StopCheckedStreams()
    LOCAL hLV AS DWORD
    LOCAL i AS LONG
    LOCAL nCount AS LONG
    LOCAL devIdx AS LONG
    LOCAL lvi AS LV_ITM
    LOCAL fNum AS LONG
    LOCAL hThread AS DWORD
    LOCAL lRes AS LONG
    LOCAL stopped AS LONG
    LOCAL sMac AS STRING
    LOCAL stopIP AS DWORD
    LOCAL stopPort AS DWORD

    CONTROL HANDLE g_hDlg, %IDC_LISTVIEW TO hLV
    IF hLV = 0 THEN EXIT SUB

    nCount = SendMessage(hLV, %LVM_GETITEMCOUNT, 0, 0)

    ' Phase 1: Signal all checked streaming devices to stop + collect fNum
    FOR i = 0 TO nCount - 1
        IF IsItemChecked(hLV, i) = 0 THEN ITERATE FOR

        lvi.mask     = %LVIF_PARAM
        lvi.iItem    = i
        lvi.iSubItem = 0
        SendMessage hLV, %LVM_GETITEM, 0, VARPTR(lvi)
        devIdx = lvi.lParam

        IF devIdx < 0 OR devIdx >= %MAX_DEVICES THEN ITERATE FOR

        EnterCriticalSection g_csDev
        IF g_Devs(devIdx).dwHBActive = 0 THEN
            LeaveCriticalSection g_csDev
            ITERATE FOR
        END IF
        ' Capture IP/Port BEFORE clearing dwHBActive for SendStop
        stopIP   = g_Devs(devIdx).dwIP
        stopPort = g_Devs(devIdx).dwPort
        g_Devs(devIdx).dwRunning  = 0
        g_Devs(devIdx).dwHBActive = 0
        g_Devs(devIdx).dwStatus   = %STS_IDLE
        LeaveCriticalSection g_csDev
        AddLog "[HB] dwHBActive=0 for dev #" & TRIM$(STR$(devIdx)) & _
               " - heartbeat STOPPED (single stop)"
        EnterCriticalSection g_csDev
        fNum = g_Devs(devIdx).fAudioFile
        g_Devs(devIdx).fAudioFile = 0
        sMac = TRIM$(g_Devs(devIdx).sMac)
        LeaveCriticalSection g_csDev

        ' Send explicit CMD_STOP to device so it stops immediately
        ' (instead of waiting for watchdog timeout)
        SendStop stopIP, stopPort

        ' Close UDP socket outside CS to avoid deadlock with AudioThread
        IF fNum THEN
            ERRCLEAR
            UDP CLOSE #fNum
        END IF

        INCR stopped
        AddLog "Stream stopping: " & sMac
    NEXT i

    ' Phase 2: Wait for all audio threads to finish
    FOR i = 0 TO nCount - 1
        IF IsItemChecked(hLV, i) = 0 THEN ITERATE FOR

        lvi.mask     = %LVIF_PARAM
        lvi.iItem    = i
        lvi.iSubItem = 0
        SendMessage hLV, %LVM_GETITEM, 0, VARPTR(lvi)
        devIdx = lvi.lParam

        IF devIdx < 0 OR devIdx >= %MAX_DEVICES THEN ITERATE FOR

        EnterCriticalSection g_csDev
        hThread = g_Devs(devIdx).hAudioThread
        g_Devs(devIdx).hAudioThread = 0
        LeaveCriticalSection g_csDev

        IF hThread THEN
            WaitForSingleObject hThread, 3000
            THREAD CLOSE hThread TO lRes
        END IF
    NEXT i
END SUB

' StopAllStreams - Stop all active streams, unblock audio UDP recv
SUB StopAllStreams()
    LOCAL idx AS LONG
    DIM fNums(%MAX_DEVICES - 1) AS LONG
    DIM stopIPs(%MAX_DEVICES - 1) AS DWORD
    DIM stopPorts(%MAX_DEVICES - 1) AS DWORD

    EnterCriticalSection g_csDev

    FOR idx = 0 TO %MAX_DEVICES - 1
        IF g_Devs(idx).dwActive AND g_Devs(idx).dwHBActive THEN
            ' Capture IP/Port for SendStop BEFORE clearing dwHBActive
            stopIPs(idx)   = g_Devs(idx).dwIP
            stopPorts(idx) = g_Devs(idx).dwPort
            g_Devs(idx).dwRunning  = 0
            g_Devs(idx).dwHBActive = 0
            g_Devs(idx).dwStatus   = %STS_IDLE
            AddLog "[HB] dwHBActive=0 for dev #" & TRIM$(STR$(idx)) & _
                   " - heartbeat STOPPED (stop all)"
        ELSE
            stopIPs(idx) = 0
        END IF
        ' Read fAudioFile under CS and zero it
        fNums(idx) = g_Devs(idx).fAudioFile
        g_Devs(idx).fAudioFile = 0
    NEXT idx

    LeaveCriticalSection g_csDev

    ' Send explicit CMD_STOP to each device OUTSIDE CS (UDP SEND can block)
    FOR idx = 0 TO %MAX_DEVICES - 1
        IF stopIPs(idx) THEN
            SendStop stopIPs(idx), stopPorts(idx)
        END IF
    NEXT idx

    ' Close audio UDP sockets OUTSIDE CS to avoid deadlock
    ' (AudioThread might be trying to enter CS when we close its socket)
    FOR idx = 0 TO %MAX_DEVICES - 1
        IF fNums(idx) THEN
            ERRCLEAR
            UDP CLOSE #fNums(idx)
        END IF
    NEXT idx
END SUB

' WaitForAllThreads - Wait for all worker threads to finish (max 3s each)
SUB WaitForAllThreads()
    LOCAL idx AS LONG
    LOCAL hThread AS DWORD
    LOCAL lRes AS LONG

    ' Wait for audio threads - read handles under CS
    FOR idx = 0 TO %MAX_DEVICES - 1
        EnterCriticalSection g_csDev
        hThread = g_Devs(idx).hAudioThread
        g_Devs(idx).hAudioThread = 0
        LeaveCriticalSection g_csDev

        IF hThread THEN
            WaitForSingleObject hThread, 3000
            THREAD CLOSE hThread TO lRes
        END IF
    NEXT idx

    ' Wait for heartbeat thread
    IF g_hHbTh THEN
        WaitForSingleObject g_hHbTh, 3000
    END IF
END SUB

' ============================================================================
'  GUI - REFRESH & CALLBACK
' ============================================================================

' WriteWavHeader - Write 44-byte WAV header to the current dump file.
' Called on first packet and when auto-splitting to a new file.
SUB WriteWavHeader()
    IF g_hDumpFile = 0 THEN EXIT SUB

    LOCAL sampleRate AS DWORD
    LOCAL channels AS LONG
    LOCAL bitsPerSample AS LONG
    LOCAL byteRate AS DWORD
    LOCAL blockAlign AS LONG
    LOCAL hdr AS STRING

    sampleRate = g_dumpSampleRate
    channels = g_dumpChannels
    bitsPerSample = g_dumpBits
    byteRate = sampleRate * channels * (bitsPerSample \ 8)
    blockAlign = channels * (bitsPerSample \ 8)

    ' Build 44-byte WAV header
    hdr = "RIFF" & _
          MKL$(36) & _              ' file size - 8 (placeholder, updated on close)
          "WAVE" & _
          "fmt " & _
          MKL$(16) & _              ' fmt chunk size
          MKI$(1) & _               ' format tag = PCM
          MKI$(channels) & _        ' channels
          MKL$(sampleRate) & _      ' sample rate
          MKL$(byteRate) & _        ' byte rate
          MKI$(blockAlign) & _      ' block align
          MKI$(bitsPerSample) & _   ' bits per sample
          "data" & _
          MKL$(0)                   ' data size (placeholder, updated on close)

    PUT$ #g_hDumpFile, hdr
    g_dumpDataSize = 0
    g_dumpWavReady = 1
END SUB

' UpdateWavHeader - Update RIFF size and data size in the WAV header.
' Called when closing a file (on stop or auto-split).
SUB UpdateWavHeader()
    IF g_hDumpFile = 0 THEN EXIT SUB

    ' Update data chunk size (offset 40)
    SEEK #g_hDumpFile, 40
    PUT$ #g_hDumpFile, MKL$(g_dumpDataSize)

    ' Update RIFF size (offset 4) = 36 + data size
    SEEK #g_hDumpFile, 4
    PUT$ #g_hDumpFile, MKL$(36 + g_dumpDataSize)

    ' Seek to end for further writes
    SEEK #g_hDumpFile, LOF(g_hDumpFile) + 1
END SUB

' OpenNewDumpFile - Open a new WAV dump file with the next index.
' Called on initial start and on 1 GB auto-split.
SUB OpenNewDumpFile()
    LOCAL sFile AS STRING
    LOCAL stDump AS SYSTEMTIME

    GetLocalTime stDump
    sFile = g_dumpBaseName & "_" & TRIM$(STR$(g_dumpFileIdx)) & ".wav"

    g_hDumpFile = FREEFILE
    OPEN sFile FOR BINARY ACCESS WRITE LOCK SHARED AS #g_hDumpFile
    IF ERR THEN
        ERRCLEAR
        AddLog "ERROR: cannot open " & sFile
        g_hDumpFile = 0
        g_bDumping = 0
        EXIT SUB
    END IF

    WriteWavHeader
    AddLog "WAV dump: " & sFile & " (" & TRIM$(STR$(g_dumpSampleRate)) & _
           "Hz " & TRIM$(STR$(g_dumpChannels)) & "ch " & _
           TRIM$(STR$(g_dumpBits)) & "-bit)"
END SUB

' ToggleDump - Start/stop WAV audio dump to file.
'   When starting: opens dump_<timestamp>_1.wav for binary write, sets g_bDumping=1.
'   The audio thread writes decoded PCM to the WAV file.
'   Files auto-split at 1 GB. When stopping: WAV header is finalized.
'   The button caption toggles between "DUMP" and "STOP DUMP".
SUB ToggleDump()
    LOCAL sFile AS STRING
    LOCAL sBtn  AS STRING

    IF g_bDumping THEN
        ' ---- Stop dumping ----
        g_bDumping = 0
        EnterCriticalSection g_csDump
        IF g_hDumpFile THEN
            UpdateWavHeader
            CLOSE g_hDumpFile
            g_hDumpFile = 0
        END IF
        LeaveCriticalSection g_csDump
        CONTROL SET TEXT g_hDlg, %IDC_BTN_DUMP, "DUMP"
        AddLog "WAV dump stopped (" & TRIM$(STR$(g_dumpFileIdx)) & " file(s))"
    ELSE
        ' ---- Start dumping ----
        LOCAL stDump AS SYSTEMTIME
        GetLocalTime stDump
        g_dumpBaseName = "dump_" & _
                RIGHT$("0" & TRIM$(STR$(stDump.wHour)), 2) & _
                RIGHT$("0" & TRIM$(STR$(stDump.wMinute)), 2) & _
                RIGHT$("0" & TRIM$(STR$(stDump.wSecond)), 2)
        g_dumpFileIdx = 0
        g_dumpWavReady = 0
        g_dumpCodec = 0
        g_dumpDataSize = 0
        g_dumpSeqCounter = 0
        ' Will open file on first packet (need format info from packet header)
        g_bDumping = 1
        CONTROL SET TEXT g_hDlg, %IDC_BTN_DUMP, "STOP DUMP"
        AddLog "WAV dump: waiting for first packet to determine format..."
    END IF
END SUB

' RefreshUI - Dynamically add/remove/update ListView items + StatusBar
'   - New devices: insert row
'   - Gone devices: remove row (log it)
'   - Existing devices: update sub-items in-place
SUB RefreshUI()
    LOCAL i AS LONG
    LOCAL lvIdx AS LONG
    LOCAL lvCount AS LONG
    LOCAL devIdx AS LONG
    LOCAL cnt AS LONG
    LOCAL strmCnt AS LONG
    LOCAL found AS LONG
    LOCAL lvi AS LV_ITM
    LOCAL szText AS ASCIIZ * 256
    LOCAL sStatus AS STRING
    LOCAL sHeap AS STRING
    LOCAL sLine AS STRING
    LOCAL selIdx AS LONG
    LOCAL tick AS DWORD
    LOCAL totalSecs AS LONG
    LOCAL mins AS LONG
    LOCAL secs AS LONG
    LOCAL hLV AS DWORD
    LOCAL sGoneMac AS STRING
    DIM goneMacs(%MAX_DEVICES - 1) AS STRING
    LOCAL goneCount AS LONG

    goneCount = 0

    ' Save current selection (device array index)
    selIdx = GetSelectedDevIdx

    ' Disable redraw to prevent flicker
    CONTROL HANDLE g_hDlg, %IDC_LISTVIEW TO hLV
    IF hLV = 0 THEN EXIT SUB    ' guard: ListView not yet created
    SendMessage hLV, %WM_SETREDRAW, 0, 0

    EnterCriticalSection g_csDev

    ' ---- Phase 1: Remove rows for devices that are no longer active ----
    lvCount = SendMessage(hLV, %LVM_GETITEMCOUNT, 0, 0)
    FOR lvIdx = lvCount - 1 TO 0 STEP -1       ' iterate backwards!
        lvi.mask    = %LVIF_PARAM
        lvi.iItem   = lvIdx
        lvi.iSubItem = 0
        SendMessage hLV, %LVM_GETITEM, 0, VARPTR(lvi)
        devIdx = lvi.lParam
        IF devIdx < 0 OR devIdx >= %MAX_DEVICES THEN
            ' Invalid lParam - remove
            SendMessage hLV, %LVM_DELETEITEM, lvIdx, 0
        ELSEIF g_Devs(devIdx).dwActive = 0 THEN
            ' Device gone - collect MAC, remove row (log AFTER leaving CS)
            goneMacs(goneCount) = TRIM$(g_Devs(devIdx).sMac)
            INCR goneCount
            SendMessage hLV, %LVM_DELETEITEM, lvIdx, 0
        END IF
    NEXT lvIdx

    ' ---- Phase 2: Update existing rows and insert new ones ----
    cnt = 0
    strmCnt = 0

    FOR i = 0 TO %MAX_DEVICES - 1
        IF g_Devs(i).dwActive = 0 THEN ITERATE FOR

        INCR cnt
        IF g_Devs(i).dwHBActive THEN INCR strmCnt

        ' Find existing ListView row for this device (match by lParam = devIdx)
        found = -1
        lvCount = SendMessage(hLV, %LVM_GETITEMCOUNT, 0, 0)
        FOR lvIdx = 0 TO lvCount - 1
            lvi.mask     = %LVIF_PARAM
            lvi.iItem    = lvIdx
            lvi.iSubItem = 0
            SendMessage hLV, %LVM_GETITEM, 0, VARPTR(lvi)
            IF lvi.lParam = i THEN
                found = lvIdx
                EXIT FOR
            END IF
        NEXT lvIdx

        IF found >= 0 THEN
            ' ---- Existing device: just update columns ----
            lvIdx = found
        ELSE
            ' ---- New device: insert row ----
            szText = TRIM$(g_Devs(i).sMac)
            lvi.mask     = %LVIF_TEXT OR %LVIF_PARAM
            lvi.iItem    = SendMessage(hLV, %LVM_GETITEMCOUNT, 0, 0)   ' append at end
            lvi.iSubItem = 0
            lvi.pszText  = VARPTR(szText)
            lvi.lParam   = i           ' store device array index
            lvIdx = SendMessage(hLV, %LVM_INSERTITEM, 0, VARPTR(lvi))
        END IF

        ' Col 1 - IP:Port
        szText = FormatIP(g_Devs(i).dwIP) & ":" & TRIM$(STR$(g_Devs(i).dwPort))
        lvi.mask     = %LVIF_TEXT
        lvi.iItem    = lvIdx
        lvi.iSubItem = %LV_COL_IP
        lvi.pszText  = VARPTR(szText)
        SendMessage hLV, %LVM_SETITEMTEXT, lvIdx, VARPTR(lvi)

        ' Col 2 - Status
        SELECT CASE g_Devs(i).dwStatus
            CASE %STS_STREAM: sStatus = "Streaming"
            CASE %STS_ERROR:  sStatus = "Error"
            CASE ELSE:        sStatus = "Idle"
        END SELECT
        szText = sStatus
        lvi.iSubItem = %LV_COL_STATUS
        lvi.pszText  = VARPTR(szText)
        SendMessage hLV, %LVM_SETITEMTEXT, lvIdx, VARPTR(lvi)

        ' Col 3 - Sample Rate
        szText = TRIM$(STR$(g_Devs(i).dwSmpRate)) & " Hz"
        lvi.iSubItem = %LV_COL_RATE
        lvi.pszText  = VARPTR(szText)
        SendMessage hLV, %LVM_SETITEMTEXT, lvIdx, VARPTR(lvi)

        ' Col 4 - Bits
        IF g_Devs(i).dwBits > 0 THEN
            szText = TRIM$(STR$(g_Devs(i).dwBits)) & "-bit"
        ELSE
            szText = "-"
        END IF
        lvi.iSubItem = %LV_COL_BITS
        lvi.pszText  = VARPTR(szText)
        SendMessage hLV, %LVM_SETITEMTEXT, lvIdx, VARPTR(lvi)

        ' Col 5 - Channels
        SELECT CASE g_Devs(i).dwChannels
            CASE 1:  szText = "mono"
            CASE 2:  szText = "stereo"
            CASE ELSE: szText = TRIM$(STR$(g_Devs(i).dwChannels))
        END SELECT
        lvi.iSubItem = %LV_COL_CH
        lvi.pszText  = VARPTR(szText)
        SendMessage hLV, %LVM_SETITEMTEXT, lvIdx, VARPTR(lvi)

        ' Col 6 - Codec
        SELECT CASE g_Devs(i).dwCodec
            CASE %CODEC_ID:     szText = "ADPCM"
            CASE %CODEC_ID_PCM: szText = "PCM 16"
            CASE ELSE:          szText = TRIM$(STR$(g_Devs(i).dwCodec))
        END SELECT
        lvi.iSubItem = %LV_COL_CODEC
        lvi.pszText  = VARPTR(szText)
        SendMessage hLV, %LVM_SETITEMTEXT, lvIdx, VARPTR(lvi)

        ' Col 7 - RSSI
        szText = STR$(g_Devs(i).dwRSSI) & " dBm"
        lvi.iSubItem = %LV_COL_RSSI
        lvi.pszText  = VARPTR(szText)
        SendMessage hLV, %LVM_SETITEMTEXT, lvIdx, VARPTR(lvi)

        ' Col 4 - Heap
        IF g_Devs(i).dwFreeHeap >= 1024 THEN
            sHeap = LTRIM$(STR$(g_Devs(i).dwFreeHeap \ 1024)) & "KB"
        ELSE
            sHeap = LTRIM$(STR$(g_Devs(i).dwFreeHeap)) & "B"
        END IF
        szText = sHeap
        lvi.iSubItem = %LV_COL_HEAP
        lvi.pszText  = VARPTR(szText)
        SendMessage hLV, %LVM_SETITEMTEXT, lvIdx, VARPTR(lvi)

        ' Col 5 - Firmware
        szText = TRIM$(g_Devs(i).sFirmware)
        lvi.iSubItem = %LV_COL_FW
        lvi.pszText  = VARPTR(szText)
        SendMessage hLV, %LVM_SETITEMTEXT, lvIdx, VARPTR(lvi)

        ' Col 6 - Packets
        szText = STR$(g_Devs(i).dwPktRecv)
        lvi.iSubItem = %LV_COL_PKTS
        lvi.pszText  = VARPTR(szText)
        SendMessage hLV, %LVM_SETITEMTEXT, lvIdx, VARPTR(lvi)

        ' Col 7 - Lost
        szText = STR$(g_Devs(i).dwPktLost)
        lvi.iSubItem = %LV_COL_LOST
        lvi.pszText  = VARPTR(szText)
        SendMessage hLV, %LVM_SETITEMTEXT, lvIdx, VARPTR(lvi)

        ' Col 8 - Duration
        IF g_Devs(i).dwHBActive THEN
            tick = CLNG(TIMER * 1000)
            totalSecs = (tick - g_Devs(i).dwStreamStart) \ 1000
            mins = totalSecs \ 60
            secs = totalSecs MOD 60
            szText = LTRIM$(STR$(mins)) & "m " & LTRIM$(STR$(secs)) & "s"
        ELSE
            szText = "-"
        END IF
        lvi.iSubItem = %LV_COL_DUR
        lvi.pszText  = VARPTR(szText)
        SendMessage hLV, %LVM_SETITEMTEXT, lvIdx, VARPTR(lvi)
    NEXT i

    LeaveCriticalSection g_csDev

    ' Log gone devices OUTSIDE CS (AddLog must not be called under CS)
    FOR i = 0 TO goneCount - 1
        AddLog "Device gone: MAC=" & goneMacs(i)
    NEXT i

    ' Restore selection by device array index
    IF selIdx >= 0 THEN
        LOCAL findIdx AS LONG
        lvCount = SendMessage(hLV, %LVM_GETITEMCOUNT, 0, 0)
        FOR findIdx = 0 TO lvCount - 1
            LOCAL chkLvi AS LV_ITM
            chkLvi.mask     = %LVIF_PARAM
            chkLvi.iItem    = findIdx
            chkLvi.iSubItem = 0
            SendMessage hLV, %LVM_GETITEM, 0, VARPTR(chkLvi)
            IF chkLvi.lParam = selIdx THEN
                chkLvi.mask      = %LVIF_STATE
                chkLvi.state     = %LVIS_SELECTED OR %LVIS_FOCUSED
                chkLvi.stateMask = %LVIS_SELECTED OR %LVIS_FOCUSED
                SendMessage hLV, %LVM_SETITEMSTATE, findIdx, VARPTR(chkLvi)
                EXIT FOR
            END IF
        NEXT findIdx
    END IF

    ' Re-enable redraw
    IF hLV THEN SendMessage hLV, %WM_SETREDRAW, 1, 0
    IF hLV THEN InvalidateRect hLV, BYVAL %NULL, %TRUE

    ' Update StatusBar
    sLine = "  EASSP Server  |  Devices:" & STR$(cnt) & "  |  Streaming:" & STR$(strmCnt) & "  |  UDP:3950"
    CONTROL SEND g_hDlg, %IDC_STATUSBAR, %SB_SETTEXT, 0, STRPTR(sLine)

    ' Update button enable/disable state
    UpdateButtonStates
END SUB

' ============================================================================
'  Main Dialog Callback
' ============================================================================
CALLBACK FUNCTION MainDlgProc()

    SELECT CASE CB.MSG

        CASE %WM_INITDIALOG
            ' Set timer for UI refresh
            SetTimer CB.HNDL, %IDT_REFRESH, %REFRESH_MS, %NULL
            ' Initial layout
            ResizeControls
            ' Initial button states (all disabled)
            UpdateButtonStates

        CASE %WM_ENTERSIZEMOVE
            ' Pause refresh timer during window move/resize
            KillTimer CB.HNDL, %IDT_REFRESH
            FUNCTION = 0
            EXIT FUNCTION

        CASE %WM_EXITSIZEMOVE
            ' Resume refresh timer and update after resize ends
            SetTimer CB.HNDL, %IDT_REFRESH, %REFRESH_MS, %NULL
            RefreshUI
            FUNCTION = 0
            EXIT FUNCTION

        CASE %WM_SIZE
            ' Resize all controls atomically FIRST - this positions
            ' StatusBar at the bottom via DeferWindowPos with SWP_NOCOPYBITS,
            ' so no BitBlt of old pixels over buttons.
            ResizeControls
            ' THEN let StatusBar recalculate its internal parts (section
            ' widths, size grip). Since ResizeControls already positioned
            ' it correctly, the StatusBar's internal SetWindowPos detects
            ' no position change and is a no-op - no BitBlt, no artifacts.
            CONTROL SEND CB.HNDL, %IDC_STATUSBAR, %WM_SIZE, CB.WPARAM, CB.LPARAM
            FUNCTION = 1
            EXIT FUNCTION

        CASE %WM_GETMINMAXINFO
            ' Set minimum window size to prevent controls from overlapping
            LOCAL pmmi AS MINMAXINFO PTR
            pmmi = CB.LPARAM
            @pmmi.ptMinTrackSize.x = 500
            @pmmi.ptMinTrackSize.y = 300
            FUNCTION = 0       ' let default processing apply the changes
            EXIT FUNCTION

        CASE %WM_TIMER
            RefreshUI
            FUNCTION = 1
            EXIT FUNCTION

        CASE %WM_COMMAND
            SELECT CASE CB.CTL
                CASE %IDC_BTN_START
                    IF CB.CTLMSG = %BN_CLICKED THEN
                        StartCheckedStreams
                        UpdateButtonStates
                        FUNCTION = 1
                    END IF

                CASE %IDC_BTN_STOP
                    IF CB.CTLMSG = %BN_CLICKED THEN
                        StopCheckedStreams
                        UpdateButtonStates
                        FUNCTION = 1
                    END IF

                CASE %IDC_BTN_STOPALL
                    IF CB.CTLMSG = %BN_CLICKED THEN
                        StopAllStreams
                        UpdateButtonStates
                        FUNCTION = 1
                    END IF

                CASE %IDC_BTN_DUMP
                    IF CB.CTLMSG = %BN_CLICKED THEN
                        ToggleDump
                        FUNCTION = 1
                    END IF
            END SELECT

        CASE %WM_NOTIFY
            LOCAL pnmh AS NMHDR PTR
            pnmh = CB.LPARAM
            ' Handle ListView checkbox state changes for immediate button update
            IF @pnmh.hwndFrom THEN
                LOCAL hLV AS DWORD
                CONTROL HANDLE CB.HNDL, %IDC_LISTVIEW TO hLV
                IF @pnmh.hwndFrom = hLV AND @pnmh.code = %LVN_ITEMCHANGED THEN
                    LOCAL uChanged AS DWORD
                    LOCAL uNewState AS DWORD
                    LOCAL uOldState AS DWORD
                    uChanged  = PEEK(DWORD, CB.LPARAM + 28)   ' NM_LISTVIEW.uChanged offset
                    uNewState = PEEK(DWORD, CB.LPARAM + 20)   ' NM_LISTVIEW.uNewState offset
                    uOldState = PEEK(DWORD, CB.LPARAM + 24)   ' NM_LISTVIEW.uOldState offset
                    IF (uChanged AND %LVIF_STATE) THEN
                        ' Checkbox state image changed? (bits 12-15)
                        IF (uNewState AND &HF000) <> (uOldState AND &HF000) THEN
                            UpdateButtonStates
                        END IF
                    END IF
                END IF
            END IF
            ' Do NOT set FUNCTION = 1 here - let other notifications
            ' (HDN_BEGINTRACK, HDN_DIVIDERDBLCLICK, etc.) pass through
            ' to default processing so column resizing works

        CASE %WM_UDP_DISC
            IF LO(WORD, CB.LPARAM) = %FD_READ THEN
                DiscoveryProc
                ' NOTE: Do NOT re-arm UDP NOTIFY here.
                ' PowerBASIC UDP NOTIFY is persistent - set once in
                ' InitDiscovery(), it stays active until UDP CLOSE.
                ' Re-arming can cause duplicate notifications or leak
                ' internal handles. Removed per PB docs & best practice.
            END IF
            FUNCTION = 1
            EXIT FUNCTION

        CASE %WM_APP_LOG
            ' FIX C5: Async log message from worker threads.
            ' CB.LPARAM = STRPTR(sLine) - a PowerBASIC string handle.
            ' PEEK$ reads the string data, then we append to the log textbox.
            IF CB.LPARAM THEN
                LOCAL sLogLine AS STRING
                sLogLine = PEEK$(CB.LPARAM, 4096)   ' read up to 4KB
                ' Find the actual end (PB strings are not NUL-terminated
                ' in memory, but PEEK$ with a max length is safe here
                ' because sLine was built with known content)
                LOCAL nLen AS LONG
                CONTROL SEND g_hDlg, %IDC_LOG, %WM_GETTEXTLENGTH, 0, 0 TO nLen
                IF nLen > 32768 THEN
                    CONTROL SET TEXT g_hDlg, %IDC_LOG, ""
                    nLen = 0
                END IF
                CONTROL SEND g_hDlg, %IDC_LOG, %EM_SETSEL, nLen, nLen
                CONTROL SEND g_hDlg, %IDC_LOG, %EM_REPLACESEL, 0, STRPTR(sLogLine)
            END IF
            FUNCTION = 1
            EXIT FUNCTION

        CASE %WM_SYSCOMMAND
            ' Per MS spec: low 4 bits of wParam are reserved by the system
            ' and must be masked off before comparing command IDs.
            ' Without masking, some Windows versions may set bits that
            ' cause the comparison to fail silently.
            IF (CB.WPARAM AND &HFFF0) = %IDM_ADD_DEVICE THEN
                ShowAddDeviceDlg CB.HNDL
                FUNCTION = 1
                EXIT FUNCTION
            END IF

            IF (CB.WPARAM AND &HFFF0) = %IDM_ABOUT THEN
                MSGBOX "EASSP Server v1.0" & $CRLF & _
                       "ESP8266 WiFi Microphone" & $CRLF & _
                       "IMA ADPCM | WaveOut | UDP:3950" & $CRLF & _
                       "PowerBASIC | Zero Dependencies", _
                       %MB_OK OR %MB_ICONINFORMATION, "About EASSP Server"
                FUNCTION = 1
                EXIT FUNCTION
            END IF

        CASE %WM_CLOSE
            g_bRunning = 0
            KillTimer CB.HNDL, %IDT_REFRESH

            ' Close dump file if open
            IF g_bDumping THEN
                g_bDumping = 0
                IF g_hDumpFile THEN
                    CLOSE g_hDumpFile
                    g_hDumpFile = 0
                END IF
            END IF

            ' Stop all streams (closes audio UDP to unblock recv)
            StopAllStreams

            ' Close discovery socket
            IF g_fDiscOpen THEN
                UDP CLOSE #g_fDiscFile
                g_fDiscOpen = 0
            END IF

            ' Wait for all threads to actually finish before destroying window
            WaitForAllThreads

    END SELECT
END FUNCTION

' ============================================================================
'  Add Device Dialog - Manual unicast DISCOVER
'
'  Allows adding a device by IP when broadcast ANNOUNCE doesn't reach
'  the server (e.g., guest WiFi network blocks broadcast but unicast
'  still works). Sends a directed DISCOVER to the specified IP:port.
'  The response INFO is handled by the normal DiscoveryProc() - the
'  device appears in the ListView automatically.
' ============================================================================

CALLBACK FUNCTION AddDeviceProc()
    SELECT CASE AS LONG CB.MSG
        CASE %WM_COMMAND
            IF CB.CTL = %IDOK THEN
                LOCAL sIP AS STRING
                LOCAL sPort AS STRING
                LOCAL targetIP AS DWORD
                LOCAL targetPort AS DWORD

                CONTROL GET TEXT CB.HNDL, %IDC_AD_IP TO sIP
                CONTROL GET TEXT CB.HNDL, %IDC_AD_PORT TO sPort

                sIP = TRIM$(sIP)
                sPort = TRIM$(sPort)

                IF LEN(sIP) = 0 THEN
                    MSGBOX "Enter device IP address", %MB_ICONWARNING, "Add Device"
                    FUNCTION = 1
                    EXIT FUNCTION
                END IF

                ' Parse IP address (dotted quad -> DWORD in network byte order)
                targetIP = inet_addr(BYCOPY sIP)
                IF targetIP = %INADDR_NONE THEN
                    MSGBOX "Invalid IP address: " & sIP, %MB_ICONWARNING, "Add Device"
                    FUNCTION = 1
                    EXIT FUNCTION
                END IF

                ' Parse port (default 3950)
                IF LEN(sPort) = 0 THEN sPort = "3950"
                targetPort = VAL(sPort)
                IF targetPort < 1 OR targetPort > 65535 THEN
                    MSGBOX "Port must be 1-65535", %MB_ICONWARNING, "Add Device"
                    FUNCTION = 1
                    EXIT FUNCTION
                END IF

                ' Send directed DISCOVER (unicast) to the device
                IF g_fDiscOpen THEN
                    SendDiscover targetIP, targetPort
                    AddLog "Manual DISCOVER sent to " & sIP & ":" & TRIM$(STR$(targetPort))
                ELSE
                    MSGBOX "Discovery socket not open", %MB_ICONWARNING, "Add Device"
                END IF

                DIALOG END CB.HNDL, 1
                FUNCTION = 1
                EXIT FUNCTION

            ELSEIF CB.CTL = %IDCANCEL THEN
                DIALOG END CB.HNDL, 0
                FUNCTION = 1
                EXIT FUNCTION
            END IF
    END SELECT
END FUNCTION

SUB ShowAddDeviceDlg(BYVAL hParent AS DWORD)
    LOCAL hDlg AS DWORD

    DIALOG NEW hParent, "Add Device", , , 200, 90, %WS_POPUP OR %WS_BORDER OR _
              %WS_CAPTION OR %WS_SYSMENU OR %DS_CENTER, %WS_EX_DLGMODALFRAME TO hDlg

    CONTROL ADD LABEL,  hDlg, -1, "Device IP:", 10, 12, 50, 10
    CONTROL ADD TEXTBOX, hDlg, %IDC_AD_IP, "10.1.30.46", 65, 10, 125, 12

    CONTROL ADD LABEL,  hDlg, -1, "Port:", 10, 32, 50, 10
    CONTROL ADD TEXTBOX, hDlg, %IDC_AD_PORT, "3950", 65, 30, 50, 12

    CONTROL ADD BUTTON, hDlg, %IDOK, "Discover", 45, 55, 50, 15, %WS_TABSTOP OR %BS_DEFPUSHBUTTON
    CONTROL ADD BUTTON, hDlg, %IDCANCEL, "Cancel", 105, 55, 50, 15, %WS_TABSTOP

    DIALOG SHOW MODAL hDlg, CALL AddDeviceProc
END SUB

' ============================================================================
'  ENTRY POINT - PBMAIN
' ============================================================================
FUNCTION PBMAIN() AS LONG
    LOCAL lResult AS LONG
    LOCAL i AS LONG
    LOCAL icex AS INIT_COMMON_CONTROLSEX

    ' Allocate arrays
    REDIM g_Devs(%MAX_DEVICES - 1) AS GLOBAL DeviceInfo
    REDIM g_StepTable(88) AS GLOBAL LONG
    REDIM g_IndexTable(7) AS GLOBAL LONG

    ' Init common controls (ListView + StatusBar)
    icex.dwSize = SIZEOF(icex)
    icex.dwICC  = %ICC_LISTVIEW_CLASSES OR %ICC_BAR_CLASSES
    InitCommonControlsEx icex

    ' Init globals
    g_bRunning = 1
    g_hHeap = GetProcessHeap()
    InitializeCriticalSection g_csDev
    InitializeCriticalSection g_csDump   ' FIX C6: dump file race protection

    ' Init IMA ADPCM Step Table
    InitStepTable

    ' ---- Create main dialog ----
    DIALOG NEW PIXELS, 0, $APP_TITLE,,, 750, 480, _
        %WS_OVERLAPPED OR %WS_CAPTION OR %WS_SYSMENU OR _
        %WS_MINIMIZEBOX OR %WS_THICKFRAME OR %WS_CLIPCHILDREN, _
        %WS_EX_CONTROLPARENT OR %WS_EX_APPWINDOW TO g_hDlg

    DIALOG DEFAULT FONT "Tahoma", 9

    ' ---- ListView (Report mode) ----
    CONTROL ADD LISTVIEW, g_hDlg, %IDC_LISTVIEW, "", _
        2, 2, 746, 280, _
        %WS_CHILD OR %WS_VISIBLE OR %WS_TABSTOP OR _
        %LVS_REPORT OR %LVS_SINGLESEL OR %LVS_SHOWSELALWAYS, _
        %WS_EX_CLIENTEDGE

    ' ---- Buttons (initially disabled until checkboxes are used) ----
    CONTROL ADD BUTTON, g_hDlg, %IDC_BTN_START,   "Start Stream", 2, 290, 90, 26
    CONTROL ADD BUTTON, g_hDlg, %IDC_BTN_STOP,    "Stop Stream",  96, 290, 90, 26
    CONTROL ADD BUTTON, g_hDlg, %IDC_BTN_STOPALL, "Stop All",    190, 290, 90, 26
    CONTROL ADD BUTTON, g_hDlg, %IDC_BTN_DUMP,    "DUMP",        284, 290, 60, 26
    CONTROL DISABLE g_hDlg, %IDC_BTN_START
    CONTROL DISABLE g_hDlg, %IDC_BTN_STOP
    CONTROL DISABLE g_hDlg, %IDC_BTN_STOPALL
    CONTROL ENABLE  g_hDlg, %IDC_BTN_DUMP

    ' ---- Log textbox ----
    CONTROL ADD TEXTBOX, g_hDlg, %IDC_LOG, "", _
        2, 320, 746, 110, _
        %WS_CHILD OR %WS_VISIBLE OR %WS_VSCROLL OR _
        %ES_MULTILINE OR %ES_READONLY OR %ES_AUTOVSCROLL, _
        %WS_EX_CLIENTEDGE

    ' Monospace font for log
    LOCAL hMono AS DWORD
    FONT NEW "Courier New", 9 TO hMono
    CONTROL SET FONT g_hDlg, %IDC_LOG, hMono

    ' ---- StatusBar ----
    CONTROL ADD STATUSBAR , g_hDlg, %IDC_STATUSBAR, "", _
        0, 0, 0, 0, _
        %WS_CHILD OR %WS_VISIBLE OR %SBARS_SIZEGRIP

    ' ---- Init ListView columns ----
    InitListView

    ' System menu: About + Add Device
    LOCAL hSysMenu AS DWORD
    hSysMenu = GetSystemMenu(g_hDlg, 0)
    AppendMenu hSysMenu, %MF_SEPARATOR, 0, BYVAL %NULL
    AppendMenu hSysMenu, %MF_STRING, %IDM_ADD_DEVICE, "Add Device..."
    AppendMenu hSysMenu, %MF_STRING, %IDM_ABOUT, "About..."

    ' ---- Init network ----
    IF InitDiscovery() = 0 THEN
        AddLog "Cannot bind UDP socket on port 3950!"
    ELSE
        THREAD CREATE HeartbeatThread(0) TO g_hHbTh
        AddLog "EASSP Server started. Listening on UDP:3950"
    END IF

    ' ---- Show dialog (MODAL with callback) ----
    DIALOG SHOW MODAL g_hDlg, CALL MainDlgProc TO lResult

    ' ---- Cleanup (threads already waited in WM_CLOSE) ----
    g_bRunning = 0

    IF g_fDiscOpen THEN
        ERRCLEAR
        UDP CLOSE #g_fDiscFile
    END IF

    ' Close heartbeat thread handle (thread already exited)
    IF g_hHbTh THEN
        THREAD CLOSE g_hHbTh TO lResult
        g_hHbTh = 0
    END IF

    ' Close any remaining audio thread handles (should be 0 by now)
    FOR i = 0 TO %MAX_DEVICES - 1
        IF g_Devs(i).hAudioThread THEN
            THREAD CLOSE g_Devs(i).hAudioThread TO lResult
            g_Devs(i).hAudioThread = 0
        END IF
    NEXT i

    DeleteCriticalSection g_csDev
    DeleteCriticalSection g_csDump   ' FIX C6

    ' FIX M5: Delete font handle (was leaking ~1KB GDI memory)
    IF hMono THEN FONT END hMono

    FUNCTION = lResult
END FUNCTION
