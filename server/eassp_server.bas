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
'    - HI() / LO() functions
'
'  Compile:  PB/Win IDE or command-line
'  Output:   eassp_server.exe
' ============================================================================

#COMPILE EXE
#DIM ALL
#OPTION VERSION5

' ============================================================================
'  RESOURCE SECTION
'  ============================================================================
'  All #RESOURCE metastatements must appear BEFORE any executable code.
'  Order within the section does not matter EXCEPT for the VERSIONINFO
'  block, which must be a contiguous sequence (VERSIONINFO → FILEFLAGS/
'  FILEVERSION/PRODUCTVERSION → STRINGINFO → VERSION$ entries).
'
'  Contents:
'    1. ICON       — app icon "Waveform" (Explorer, taskbar, alt-tab)
'    2. MANIFEST   — Common Controls v6 (visual styles) + Win10/11 compat
'                    + DPI-unaware (Windows virtualizes for HiDPI)
'    3. VERSIONINFO — file/product version + standard string metadata
'                    (shown in Explorer → Properties → Details)
' ============================================================================

' ---- 1. Icon (resource ID 100) ----
' "Waveform" — circular frame + emerald sine wave + amber peak dot.
' Multi-resolution ICO (16/32/48/64/128/256). Assigned to the dialog at
' runtime via DIALOG SET ICON CB.HNDL, "#100" in WM_INITDIALOG.
#RESOURCE ICON, 100, "eassp_server.ico"

' ---- 2. Manifest (resource ID 1 = RT_MANIFEST) ----
' Enables Windows XP+ visual styles (themed buttons/controls instead of
' Win95 classic look) via Common Controls v6 dependency. Also declares
' Windows 10/11 compatibility and DPI-unaware status (Windows DPI-
' virtualizes the fixed-pixel DDT layout for correct sizing on HiDPI).
#RESOURCE MANIFEST, 1, "eassp_server.manifest"

' ---- 3. Version Info block ----
' Shown in Explorer → right-click → Properties → Details tab.
' FILEVERSION/PRODUCTVERSION are binary (4 x 16-bit); the VERSION$ strings
' are the human-readable display values.
'
' Version scheme:
'   FILEVERSION    1,0,0,0  — server exe version (independent of firmware)
'   PRODUCTVERSION 2,0,0,0  — product version (matches firmware v2.0)
'
' STRINGINFO "0419","04E4":
'   0419 = Russian (matches UI language)
'   04E4 = Windows Multilingual (covers Cyrillic + Latin)
#RESOURCE VERSIONINFO
#RESOURCE FILEFLAGS      0
#RESOURCE FILEVERSION    1, 0, 0, 0
#RESOURCE PRODUCTVERSION 2, 0, 0, 0
#RESOURCE STRINGINFO     "0419", "04E4"
#RESOURCE VERSION$ "Comments",         "ESP8266 WiFi Microphone Receiver"
#RESOURCE VERSION$ "CompanyName",      "EASSP"
#RESOURCE VERSION$ "FileDescription",  "EASSP Server - ESP8266 WiFi Microphone"
#RESOURCE VERSION$ "FileVersion",      "1.0.0.0"
#RESOURCE VERSION$ "InternalName",     "eassp_server"
#RESOURCE VERSION$ "LegalCopyright",   "Copyright (c) 2024 EASSP Project"
#RESOURCE VERSION$ "OriginalFilename", "eassp_server.exe"
#RESOURCE VERSION$ "ProductName",      "EASSP Server"
#RESOURCE VERSION$ "ProductVersion",   "2.0 (firmware v2.0)"

' ---- Win32 API includes ----
$INCLUDE "WIN32API.INC"
' FIX (AUDIT-WINAPI): WinSock2.inc is REQUIRED for inet_addr, htons,
' %INADDR_NONE and %FD_READ (used by UDP NOTIFY lParam decoding per
' PBWin.txt). Win32Api.inc v10.01.0019 does NOT transitively include
' WinSock2.inc / ws2def.inc, and WinSock.inc (v1) is an empty stub
' ("[not translated at this time]"). WinSock2.inc itself includes
' ws2def.inc, so this single line is sufficient. Per WinSock2.inc:14,
' it MUST appear AFTER $INCLUDE "Win32API.INC".
$INCLUDE "WINSOCK2.INC"

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
' FIX (GROK-17): verbose-logging flag. When 0 (default), suppresses the
' per-heartbeat "[HB] iter=... sent N DISCOVER" log that fires every 3s
' during streaming (noisy in production). Set to 1 via a future AT/cmd
' or INI key to re-enable for diagnostics. The idle branch (every 5th
' iteration) still logs regardless.
GLOBAL g_bVerbose   AS LONG

' ---- WaveOut device selection ----
' FIX (BUG#10): removed dead global g_WaveDeviceId. It was declared and
' initialized to -1 but never read - per-device selection uses
' g_Devs(idx).dwWaveDevice instead. Keeping a misleading dead global is a
' maintenance hazard (future devs might think it controls something).
GLOBAL g_sIniFile   AS STRING   ' Path to eassp_server.ini (next to .exe)

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
GLOBAL g_dumpIp     AS DWORD        ' FIX (GROK-9): now USED - set in ToggleDump to the selected device's IP. 0 = dump all (legacy). AudioThread dump paths filter by this.
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
    '
    ' FIX (use-after-free): The local sLine is destroyed when AddLog returns.
    ' PostMessage only stores a raw DWORD (the pointer value) — PowerBASIC does
    ' NOT know the pointer is referenced by the message and does NOT increment
    ' the string's reference count. By the time WM_APP_LOG is processed, the
    ' string data has been freed → dangling pointer → garbage or crash.
    ' Solution: allocate a heap copy (NUL-terminated), pass its pointer in
    ' lParam and the byte length in wParam. The GUI thread reads exactly that
    ' many bytes, then frees the heap block.
    LOCAL nLen  AS LONG
    LOCAL pBuf  AS BYTE PTR
    nLen = LEN(sLine)
    pBuf = HeapAlloc(g_hHeap, 0, nLen + 1)
    IF pBuf THEN
        POKE$ pBuf, sLine
        @pBuf[nLen] = 0          ' NUL terminator
        ' FIX (H18): check PostMessage return. If it fails (queue full, GUI
        ' being destroyed), the WM_APP_LOG handler that calls HeapFree never
        ' runs -> pBuf leaks. Free it here on failure.
        IF PostMessage(g_hDlg, %WM_APP_LOG, nLen, pBuf) = 0 THEN
            HeapFree g_hHeap, 0, pBuf
        END IF
    END IF
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

' ---- INI file persistence (per-device output routing) ----
' Uses Windows API: WritePrivateProfileString / GetPrivateProfileInt
' INI format:
'   [devices]
'   9C9C1F8DAAC5=-1     (MAC without colons = WaveOut device ID, -1 = default)
'   9C9C1F8DAABE=1

' SaveDeviceOutput - Save a single device's WaveOut ID to INI
SUB SaveDeviceOutput(BYVAL sMac AS STRING, BYVAL devId AS LONG)
    LOCAL sKey AS STRING
    ' Remove colons from MAC: "9C:9C:1F:8D:AA:C5" → "9C9C1F8DAAC5"
    sKey = REMOVE$(sMac, ":")
    IF LEN(g_sIniFile) = 0 THEN EXIT SUB
    WritePrivateProfileString "devices", BYCOPY sKey, BYCOPY TRIM$(STR$(devId)), BYCOPY g_sIniFile
END SUB

' LoadDeviceOutput - Load a device's WaveOut ID from INI (returns -1 if not found)
FUNCTION LoadDeviceOutput(BYVAL sMac AS STRING) AS LONG
    LOCAL sKey AS STRING
    sKey = REMOVE$(sMac, ":")
    IF LEN(g_sIniFile) = 0 THEN FUNCTION = -1 : EXIT FUNCTION
    FUNCTION = GetPrivateProfileInt("devices", BYCOPY sKey, -1, BYCOPY g_sIniFile)
END FUNCTION

' ============================================================================
'  Manual device persistence (Add Device dialog entries)
'  ============================================================================
'  INI format:
'    [manual_devices]
'    1=192.168.43.186:3950
'    2=10.1.30.46:3950
'    count=2
'
'  When the user adds a device via "Add Device" (manual unicast DISCOVER),
'  the IP:port is saved here. On next program start, all saved entries are
'  read and a DISCOVER is sent to each — if the device is online, it appears
'  in the ListView automatically (same flow as Add Device).
'
'  [add_device]
'  last_ip=192.168.43.186
'  last_port=3950
'
'  Last entered values in the Add Device dialog, pre-filled on next open.
' ============================================================================

' ManualDeviceAdd - Append an IP:port to the [manual_devices] section.
' Deduplicates by IP (if same IP already saved, updates its port).
SUB ManualDeviceAdd(BYVAL sIP AS STRING, BYVAL nPort AS LONG)
    IF LEN(g_sIniFile) = 0 THEN EXIT SUB
    IF LEN(sIP) = 0 THEN EXIT SUB

    LOCAL i AS LONG, n AS LONG
    LOCAL sEntry AS STRING, sExisting AS STRINGZ*256, sIPOnly AS STRING
    LOCAL bFound AS LONG

    n = GetPrivateProfileInt("manual_devices", "count", 0, BYCOPY g_sIniFile)
    ' FIX (M37): clamp to a sane upper bound. A corrupted or malicious INI
    ' with count=1000000 would iterate a million times (slow startup DoS).
    IF n > 64 THEN n = 64
    bFound = 0
    ' Check for duplicate IP (update port if found)
    FOR i = 1 TO n
        sExisting = SPACE$(256)
        GetPrivateProfileString "manual_devices", BYCOPY TRIM$(STR$(i)), "", BYREF sExisting, 256, BYCOPY g_sIniFile
        sExisting = TRIM$(sExisting, ANY CHR$(0, 32))
        ' Extract IP part (before ":")
        LOCAL colon AS LONG
        colon = INSTR(sExisting, ":")
        IF colon > 0 THEN
            sIPOnly = LEFT$(sExisting, colon - 1)
        ELSE
            sIPOnly = sExisting
        END IF
        IF sIPOnly = sIP THEN
            ' Same IP — update this entry with new port
            sEntry = sIP & ":" & TRIM$(STR$(nPort))
            WritePrivateProfileString "manual_devices", BYCOPY TRIM$(STR$(i)), _
                                       BYCOPY sEntry, BYCOPY g_sIniFile
            bFound = 1
            EXIT FOR
        END IF
    NEXT i

    IF bFound = 0 THEN
        ' New entry — append
        INCR n
        sEntry = sIP & ":" & TRIM$(STR$(nPort))
        WritePrivateProfileString "manual_devices", BYCOPY TRIM$(STR$(n)), _
                                   BYCOPY sEntry, BYCOPY g_sIniFile
        WritePrivateProfileString "manual_devices", "count", _
                                   BYCOPY TRIM$(STR$(n)), BYCOPY g_sIniFile
        AddLog "[INI] Saved manual device #" & TRIM$(STR$(n)) & ": " & sEntry
    END IF
END SUB

' ManualDeviceLoadAll - Read all saved manual devices and send DISCOVER to each.
' Called on startup (WM_INITDIALOG) after discovery socket is open.
SUB ManualDeviceLoadAll()
    IF LEN(g_sIniFile) = 0 THEN EXIT SUB
    IF g_fDiscOpen = 0 THEN EXIT SUB

    LOCAL i AS LONG, n AS LONG
    LOCAL sEntry AS STRINGZ*256, sIP AS STRING, sPort AS STRING
    LOCAL colon AS LONG, nPort AS LONG, targetIP AS DWORD
    LOCAL sentCount AS LONG

    n = GetPrivateProfileInt("manual_devices", "count", 0, BYCOPY g_sIniFile)
    ' FIX (M37): clamp to a sane upper bound. A corrupted or malicious INI
    ' with count=1000000 would iterate a million times (slow startup DoS).
    IF n > 64 THEN n = 64
    IF n = 0 THEN EXIT SUB

    AddLog "[INI] Loading " & TRIM$(STR$(n)) & " saved manual device(s) from INI..."

    FOR i = 1 TO n
        sEntry = SPACE$(256)
        GetPrivateProfileString "manual_devices", BYCOPY TRIM$(STR$(i)), _
                                 "", BYREF sEntry, 256, BYCOPY g_sIniFile
        sEntry = TRIM$(sEntry, ANY CHR$(0, 32))
        IF LEN(sEntry) = 0 THEN ITERATE FOR

        ' Parse "IP:port"
        colon = INSTR(sEntry, ":")
        IF colon > 0 THEN
            sIP = LEFT$(sEntry, colon - 1)
            sPort = MID$(sEntry, colon + 1)
        ELSE
            sIP = sEntry
            sPort = "3950"
        END IF

        ' Validate + send DISCOVER
        targetIP = inet_addr(BYCOPY sIP)
        IF targetIP <> %INADDR_NONE THEN
            nPort = VAL(sPort)
            IF nPort < 1 OR nPort > 65535 THEN nPort = 3950
            SendDiscover targetIP, nPort
            INCR sentCount
            AddLog "[INI]   #" & TRIM$(STR$(i)) & ": " & sIP & ":" & TRIM$(STR$(nPort)) & " - DISCOVER sent"
        END IF
    NEXT i

    IF sentCount > 0 THEN
        AddLog "[INI] Sent DISCOVER to " & TRIM$(STR$(sentCount)) & _
               " saved device(s). Waiting for INFO responses..."
    END IF
END SUB

' SaveAddDeviceDefaults - Save last IP/port entered in Add Device dialog.
SUB SaveAddDeviceDefaults(BYVAL sIP AS STRING, BYVAL sPort AS STRING)
    IF LEN(g_sIniFile) = 0 THEN EXIT SUB
    WritePrivateProfileString "add_device", "last_ip", BYCOPY sIP, BYCOPY g_sIniFile
    WritePrivateProfileString "add_device", "last_port", BYCOPY sPort, BYCOPY g_sIniFile
END SUB

' LoadAddDeviceDefaultIP - Returns last IP or default "10.1.30.46" if none saved.
FUNCTION LoadAddDeviceDefaultIP() AS STRING
    IF LEN(g_sIniFile) = 0 THEN FUNCTION = "10.1.30.46" : EXIT FUNCTION
    LOCAL s AS STRINGZ*64
    GetPrivateProfileString "add_device", "last_ip", "10.1.30.46",BYREF s, 64, BYCOPY g_sIniFile
    FUNCTION = TRIM$(s, ANY CHR$(0, 32))
END FUNCTION

' LoadAddDeviceDefaultPort - Returns last port or "3950" if none saved.
FUNCTION LoadAddDeviceDefaultPort() AS STRING
    IF LEN(g_sIniFile) = 0 THEN FUNCTION = "3950" : EXIT FUNCTION
    LOCAL s AS STRINGZ*16
    GetPrivateProfileString "add_device", "last_port", "3950", _
                             BYREF s, 16, BYCOPY g_sIniFile
    FUNCTION = TRIM$(s, ANY CHR$(0, 32))
END FUNCTION

' PopulateDeviceCombo - Enumerate WaveOut devices and fill the ComboBox.
' Adds "Default (WAVE_MAPPER)" as first entry, then all physical/virtual
' devices (speakers, VB-Cable, etc.). User selects which device receives
' the audio — select "CABLE Input" to route to a virtual microphone.
SUB PopulateDeviceCombo()
    LOCAL numDevs AS LONG
    LOCAL i AS LONG
    LOCAL caps AS WAVEOUTCAPS
    LOCAL sName AS STRING
    LOCAL mmRes AS LONG

    ' Start with "Default" (WAVE_MAPPER = -1)
    COMBOBOX ADD g_hDlg, %IDC_COMBO_DEVICE, "Default (WAVE_MAPPER)"

    ' Enumerate all WaveOut devices
    numDevs = waveOutGetNumDevs()
    FOR i = 0 TO numDevs - 1
        mmRes = waveOutGetDevCaps(i, caps, SIZEOF(caps))
        IF mmRes = 0 THEN
            sName = EXTRACT$(caps.szPname, CHR$(0))
            IF LEN(sName) = 0 THEN sName = "Device " & TRIM$(STR$(i))
            COMBOBOX ADD g_hDlg, %IDC_COMBO_DEVICE, _
                TRIM$(STR$(i)) & ": " & sName
        END IF
    NEXT i

    ' Select "Default" (index 0 in the combo = WAVE_MAPPER)
    COMBOBOX SELECT g_hDlg, %IDC_COMBO_DEVICE, 1
    ' FIX (BUG#10): removed dead assignment g_WaveDeviceId = -1
    ' (the global itself was removed from the declarations section)
END SUB

' InitListView - Create columns in the ListView
SUB InitListView()
    LOCAL lvc AS LV_COL
    LOCAL szText AS ASCIIZ * 64

    lvc.mask = %LVCF_FMT OR %LVCF_WIDTH OR %LVCF_TEXT
    lvc.fmt  = %LVCFMT_LEFT

    ' Col 0 - Hostname (MAC)
    szText = "Name (MAC)"  : lvc.pszText = VARPTR(szText) : lvc.cx = 200
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

' GetSelectedDevIdx - Get device array index for the focused ListView item.
' FIX (BUG#8): previously used LVM_GETNEXTITEM+LVNI_SELECTED (state-based),
' while GetSelectedDeviceIdx below used LVM_GETSELECTIONMARK (focus-based).
' In a multi-select ListView with checkboxes, LVNI_SELECTED returns the
' FIRST item with LVIS_SELECTED state (could be any checked item), while
' GETSELECTIONMARK returns the focus item (the one right-clicked for context
' menu). They diverge when the user clicks checkboxes without changing focus.
' Callers of GetSelectedDevIdx (RefreshUI column update) expected the focus
' item, not a random checked item. Unified on GETSELECTIONMARK for both.
FUNCTION GetSelectedDevIdx() AS LONG
    LOCAL hLV AS DWORD
    LOCAL lvIdx AS LONG
    LOCAL lvi AS LV_ITM
    LOCAL lResult AS LONG

    CONTROL HANDLE g_hDlg, %IDC_LISTVIEW TO hLV
    IF hLV = 0 THEN FUNCTION = -1 : EXIT FUNCTION

    lvIdx = SendMessage(hLV, %LVM_GETSELECTIONMARK, 0, 0)
    IF lvIdx < 0 THEN
        FUNCTION = -1
        EXIT FUNCTION
    END IF

    lvi.mask   = %LVIF_PARAM
    lvi.iItem  = lvIdx
    lvi.iSubItem = 0
    SendMessage hLV, %LVM_GETITEM, 0, VARPTR(lvi) TO lResult
    IF lvi.lParam >= 0 AND lvi.lParam < %MAX_DEVICES THEN
        FUNCTION = lvi.lParam
    ELSE
        FUNCTION = -1
    END IF
END FUNCTION

' IsItemChecked - Check if ListView item checkbox is checked
FUNCTION IsItemChecked(BYVAL hLV AS DWORD, BYVAL iItem AS LONG) AS LONG
    LOCAL dwState AS DWORD
    dwState = SendMessage(hLV, %LVM_GETITEMSTATE, iItem, %LVIS_STATEIMAGEMASK)
    ' State image index 2 = checked → bits 12-15 = 2 → &H2000
    FUNCTION = (dwState AND &H2000) = &H2000
END FUNCTION

' SetItemChecked - Set ListView item checkbox state
' bCheck: 1 = checked (state image 2), 0 = unchecked (state image 1)
SUB SetItemChecked(BYVAL hLV AS DWORD, BYVAL iItem AS LONG, BYVAL bCheck AS LONG)
    LOCAL lvi AS LV_ITM
    lvi.mask      = %LVIF_STATE
    lvi.stateMask = %LVIS_STATEIMAGEMASK
    IF bCheck THEN
        lvi.state = &H2000   ' state image index 2 = checked
    ELSE
        lvi.state = &H1000   ' state image index 1 = unchecked
    END IF
    SendMessage hLV, %LVM_SETITEMSTATE, iItem, VARPTR(lvi)
END SUB

' GetSelectedDeviceIdx - DEPRECATED alias for GetSelectedDevIdx.
' FIX (GROK-11): previously this was a functionally-identical duplicate
' of GetSelectedDevIdx (same LVM_GETSELECTIONMARK + LVM_GETITEM logic,
' only variable names differed). The duplication risked edit-desync:
' a future fix applied to one but not the other would silently diverge.
' Now it's a thin wrapper that delegates to the canonical implementation.
' Existing callers don't need to change. New code should call
' GetSelectedDevIdx directly.
FUNCTION GetSelectedDeviceIdx() AS LONG
    FUNCTION = GetSelectedDevIdx()
END FUNCTION

' ===========================================================================
' SetDeviceWaveOutput - CS-safe write to g_Devs(devIdx).dwWaveDevice.
' FIX (BUG#3): previously the GUI thread wrote dwWaveDevice directly
' from 4 call sites (combo box CBN_SELCHANGE, context menu Default, and
' the two specific-device variants) WITHOUT holding g_csDev. Meanwhile
' AudioThread reads dwWaveDevice for every waveOutOpen (startup, TCP
' reconnect, format-change reopen). A write mid-read is technically
' atomic on x86 (32-bit aligned LONG), but on PB the field is a LONG
' inside a TYPE - alignment is not guaranteed by the spec. Worse, the
' post-write AddLog reads g_Devs().sHostname outside CS, racing with
' UpdateDevice which can rewrite sHostname on the next INFO. This helper
' does the write + snapshot under CS, then emits log + SaveDeviceOutput
' outside CS. Returns the snapshotted hostname via BYREF for the caller's
' log line.
' ===========================================================================
SUB SetDeviceWaveOutput(BYVAL devIdx AS LONG, BYVAL newDev AS LONG)
    LOCAL saveMac AS STRING
    LOCAL saveHost AS STRING
    LOCAL sDev AS STRING

    EnterCriticalSection g_csDev
    g_Devs(devIdx).dwWaveDevice = newDev
    saveMac  = g_Devs(devIdx).sMac
    saveHost = g_Devs(devIdx).sHostname
    LeaveCriticalSection g_csDev

    SaveDeviceOutput saveMac, newDev

    IF newDev = -1 THEN
        sDev = "Default (WAVE_MAPPER)"
    ELSE
        sDev = TRIM$(STR$(newDev)) & " (will apply on next stream start)"
    END IF
    AddLog "Output device for " & TRIM$(saveHost) & ": " & sDev
END SUB

' UpdateButtonStates - Enable/disable buttons based on checkbox & stream state
' FIX (GROK-7): snapshot g_Devs under a single brief CS, then do all
' ListView reads + button updates outside CS. Previously held g_csDev
' across up to 16 LVM_GETITEM SendMessage calls.
SUB UpdateButtonStates()
    LOCAL hLV AS DWORD
    LOCAL i AS LONG
    LOCAL nCount AS LONG
    LOCAL bCanStart AS LONG
    LOCAL bCanStop AS LONG
    LOCAL bHasStream AS LONG
    LOCAL devIdx AS LONG
    LOCAL lvi AS LV_ITM
    ' FIX (GROK-7): local snapshot for CS-free ListView reads.
    DIM snap(%MAX_DEVICES - 1) AS LOCAL DeviceInfo

    CONTROL HANDLE g_hDlg, %IDC_LISTVIEW TO hLV
    IF hLV = 0 THEN EXIT SUB

    nCount = SendMessage(hLV, %LVM_GETITEMCOUNT, 0, 0)

    ' FIX (GROK-7): single brief CS to snapshot g_Devs -> snap[]
    EnterCriticalSection g_csDev
    FOR i = 0 TO %MAX_DEVICES - 1
        snap(i) = g_Devs(i)
    NEXT i
    LeaveCriticalSection g_csDev

    ' All ListView reads + button state computation now CS-free
    FOR i = 0 TO nCount - 1
        lvi.mask     = %LVIF_PARAM
        lvi.iItem    = i
        lvi.iSubItem = 0
        SendMessage hLV, %LVM_GETITEM, 0, VARPTR(lvi)
        devIdx = lvi.lParam

        IF devIdx < 0 OR devIdx >= %MAX_DEVICES THEN ITERATE FOR

        IF IsItemChecked(hLV, i) THEN
            IF snap(devIdx).dwActive AND snap(devIdx).dwHBActive = 0 THEN bCanStart = 1
            IF snap(devIdx).dwHBActive THEN bCanStop = 1
        END IF

        IF snap(devIdx).dwHBActive THEN bHasStream = 1
    NEXT i

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
    LOCAL hCombo AS DWORD
    LOCAL hLabel AS DWORD
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
    CONTROL HANDLE g_hDlg, %IDC_COMBO_DEVICE TO hCombo
    CONTROL HANDLE g_hDlg, %IDC_LBL_OUTPUT   TO hLabel

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
    hDWP = BeginDeferWindowPos(9)
    IF hDWP THEN
        ' ListView: x=2, y=2
        hDWP = DeferWindowPos(hDWP, hLV, %NULL, 2, 2, cx - 4, lvH, dwFlags)

        ' Log: x=2, y=lvH+4
        y = lvH + 4
        hDWP = DeferWindowPos(hDWP, hLog, %NULL, 2, y, cx - 4, logH, dwFlags)

        ' Button row: y=lvH+logH+8
        '   Start(2,90) Stop(96,90) DUMP(190,90) "Output:"(286) ComboBox(328) StopAll(cx-92,90)
        y = lvH + logH + 8
        hDWP = DeferWindowPos(hDWP, hBtn1, %NULL, 2, y, 90, btnH, dwFlags)
        hDWP = DeferWindowPos(hDWP, hBtn2, %NULL, 96, y, 90, btnH, dwFlags)
        hDWP = DeferWindowPos(hDWP, hBtn4, %NULL, 190, y, 90, btnH, dwFlags)
        hDWP = DeferWindowPos(hDWP, hLabel, %NULL, 286, y + 6, 40, 20, dwFlags)
        ' ComboBox: x=328, width = cx-328-92-10 = cx-430 (stretch to Stop All)
        hDWP = DeferWindowPos(hDWP, hCombo, %NULL, 328, y + 3, cx - 430, 200, dwFlags)
        hDWP = DeferWindowPos(hDWP, hBtn3, %NULL, cx - 92, y, 90, btnH, dwFlags)

        ' StatusBar: position it at the bottom, full width, fixed height.
        ' Included in DeferWindowPos batch with SWP_NOCOPYBITS to prevent
        ' its internal BitBlt from smearing old pixels over buttons.
        y = cy - statusH
        hDWP = DeferWindowPos(hDWP, hStatus, %NULL, 0, y, cx, statusH, dwFlags)

        EndDeferWindowPos hDWP
    END IF

    ' Recalculate StatusBar part widths for new window width.
    ' 5 parts: [0] EASSP Server  [1] Devices  [2] Streaming  [3] UDP  [4] Output (stretch)
    DIM sbP(0 TO 4) AS LONG
    sbP(0) = 100
    sbP(1) = 180
    sbP(2) = 270
    sbP(3) = 350
    sbP(4) = -1   ' remaining width
    CONTROL SEND g_hDlg, %IDC_STATUSBAR, %SB_SETPARTS, 5, VARPTR(sbP(0))
END SUB

' ============================================================================
'  NETWORK FUNCTIONS (using PowerBASIC built-in UDP)
' ============================================================================

' InitDiscovery - Open UDP socket on port 3950
FUNCTION InitDiscovery() AS LONG
    LOCAL fNum AS LONG

    fNum = FREEFILE
    ERRCLEAR

    ' FIX (AUDIT-W1): timeout was 60000 (60s). DiscoveryProc runs UDP RECV
    ' in the GUI thread via the WM_UDP_DISC handler. If a datagram is lost
    ' between FD_READ firing and RECV executing (rare race), the GUI would
    ' freeze for up to 60s. 2000ms is plenty for a localhost/LAN datagram
    ' that the OS already buffered; if it really is gone, we just exit
    ' DiscoveryProc and wait for the next FD_READ notification.
    UDP OPEN PORT 3950 AS #fNum TIMEOUT 2000

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
    ' FIX (L37): correct byte counts. v2.0=34 bytes (no transport_mode,
    ' no hostname), v2.1=35 (+1 transport_mode), v2.2=58 (+24 hostname).
    ' Accept INFO payloads from v2.0 (34 bytes) through v2.2 (58 bytes).
    ' Each field is read with its own bufLen guard, so shorter payloads
    ' from older firmware are handled gracefully (missing fields default).
    ' FIX (BUG#R2-2): was hdr+33, but UpdateDevice accepts hdr+32 (INFO v1
    ' payload without bits_per_sample). A v1 device sending exactly 32-byte
    ' payload (40 bytes total) would be rejected here before UpdateDevice ever
    ' saw it - silently invisible in the device list. Aligned with UpdateDevice's
    ' minimum. Older firmware reporting only status/codec/error/ch/rate/frame_ms/
    ' mac/pktsSent/freeHeap/RSSI (32 bytes) now discovers; bits defaults to 16.
    IF LEN(recvBuf) < %EASSP_HDR_SZ + 32 THEN EXIT SUB

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
    ' FIX (AUTOTRANSPORT): capture previous transport so we can detect a
    ' runtime mode change (UDP<->TCP) and auto-restart the audio thread.
    ' prevTransport is only meaningful when bNew=0 (existing device).
    LOCAL prevTransport AS LONG
    LOCAL bTransportChanged AS LONG
    prevTransport = -1

    b = pBuf

    ' ---- HARDENING: validate minimum INFO payload length ----
    ' The INFO payload must be at least 32 bytes (INFO v1) for the fields we
    ' read at offsets up to 31 (RSSI). Reject shorter packets — they're either
    ' a truncated/malformed INFO or a non-INFO packet that slipped through.
    ' Without this, PEEK at pBuf+12/23/27/31 could read past the buffer end.
    IF bufLen < %EASSP_HDR_SZ + 32 THEN
        EXIT SUB
    END IF

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

    tick = GetTickCount()

    IF bNew THEN
        g_Devs(idx).dwActive = 1
        g_Devs(idx).sMac = sMac
        g_Devs(idx).dwDiscovered = tick
        ' Load saved output device from INI (or -1 = default)
        g_Devs(idx).dwWaveDevice = LoadDeviceOutput(sMac)
    ELSE
        ' FIX (AUTOTRANSPORT): capture old transport before INFO parse
        ' overwrites it. Used after CS release to detect a mode switch.
        prevTransport = g_Devs(idx).dwTransport
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
    ' transport_mode at payload offset 33 = pBuf+41 (INFO v2.1, 34-byte payload).
    ' 0=UDP, 1=TCP (ESP=listener, server connects), 2=Raw 802.11 TX.
    ' Guard against older 33-byte INFO (defaults to UDP=0).
    IF bufLen >= %EASSP_HDR_SZ + 34 THEN
        g_Devs(idx).dwTransport = PEEK(BYTE, pBuf + 41)   ' transport_mode
    ELSE
        g_Devs(idx).dwTransport = 0   ' assume UDP for old firmware
    END IF

    ' ---- HARDENING (zero-day defense): validate all fields from ESP ----
    ' The ESP is an external device on the network; a malformed INFO payload
    ' (buggy firmware, bit-flip in WiFi, or a malicious device spoofing EASSP)
    ' could carry out-of-range values that crash the server or corrupt state.
    ' Clamp/reject here at the parse boundary so bad data never propagates.
    LOCAL vCodec AS LONG, vCh AS LONG, vBits AS LONG, vRate AS DWORD, vTr AS LONG
    vCodec = g_Devs(idx).dwCodec
    vCh    = g_Devs(idx).dwChannels
    vBits  = g_Devs(idx).dwBits
    vRate  = g_Devs(idx).dwSmpRate
    vTr    = g_Devs(idx).dwTransport

    ' Codec: only 5 (ADPCM) and 6 (PCM) supported. Unknown -> default to ADPCM (5)
    IF vCodec <> %CODEC_ID AND vCodec <> %CODEC_ID_PCM THEN
        IF bNew THEN
            AddLog "[INFO] dev #" & TRIM$(STR$(idx)) & _
                   ": unsupported codec=" & TRIM$(STR$(vCodec)) & _
                   " -> defaulting to ADPCM (5)"
        END IF
        vCodec = %CODEC_ID
    END IF

    ' Channels: 1 (mono) or 2 (stereo) only. Clamp, don't reject (graceful).
    IF vCh < 1 THEN vCh = 1
    IF vCh > 2 THEN vCh = 2

    ' Bits: 16 or 24 only. Unknown -> 16 (safe default, always supported).
    IF vBits <> 16 AND vBits <> 24 THEN vBits = 16

    ' Sample rate: must be one of the 7 known EASSP rates. Unknown -> 48000.
    IF vRate <> 8000 AND vRate <> 11025 AND vRate <> 16000 AND _
       vRate <> 22050 AND vRate <> 32000 AND vRate <> 44100 AND _
       vRate <> 48000 THEN
        IF bNew THEN
            AddLog "[INFO] dev #" & TRIM$(STR$(idx)) & _
                   ": unsupported sample_rate=" & TRIM$(STR$(vRate)) & _
                   " -> defaulting to 48000"
        END IF
        vRate = 48000
    END IF

    ' Transport: 0 (UDP) or 1 (TCP) only. 2 (Raw 802.11 TX) is NOT receivable
    ' by the server (needs Npcap in monitor mode). Unknown/Raw -> default to
    ' UDP (0) so at least the server tries to listen; if it was Raw, no audio
    ' will arrive and stall-detection will log it (better than silent refusal).
    IF vTr <> 0 AND vTr <> 1 THEN
        IF bNew AND vTr <> 0 THEN
            AddLog "[INFO] dev #" & TRIM$(STR$(idx)) & _
                   ": unsupported transport=" & TRIM$(STR$(vTr)) & _
                   " -> defaulting to UDP (0). Raw TX (2) cannot be received."
        END IF
        vTr = 0
    END IF

    g_Devs(idx).dwCodec     = vCodec
    g_Devs(idx).dwChannels  = vCh
    g_Devs(idx).dwBits      = vBits
    g_Devs(idx).dwSmpRate   = vRate
    g_Devs(idx).dwTransport = vTr

    ' FIX (AUTOTRANSPORT): if this is an existing device whose stream is
    ' currently active and the ESP just reported a different transport_mode,
    ' flag it for restart after we release the CS. We restart from outside
    ' the CS to avoid holding it during StopCheckedStreams/StartCheckedStreams
    ' (which themselves enter/leave the CS, and THREAD CREATE + socket open
    ' can block - never hold CS during blocking operations).
    IF bNew = 0 AND prevTransport >= 0 AND prevTransport <> vTr AND _
       g_Devs(idx).dwHBActive <> 0 THEN
        bTransportChanged = 1
    END IF

    ' v2.2: hostname at payload offset 34 = pBuf+42 (INFO v2.2, 58-byte payload).
    ' 24 bytes, NUL-terminated. Guard against older payloads (empty string).
    IF bufLen >= %EASSP_HDR_SZ + 58 THEN
        g_Devs(idx).sHostname = PEEK$(pBuf + 42, 24)
    ELSE
        g_Devs(idx).sHostname = ""
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

    ' FIX (BUG#7): snapshot sHostname and dwSmpRate INSIDE CS so the
    ' post-CS log lines don't race with the next UpdateDevice call
    ' (which can rewrite these fields on the next INFO packet from a
    ' different device, since UpdateDevice is reentrant via DiscoveryProc).
    LOCAL saveHostname AS STRING
    LOCAL saveSmpRate AS DWORD
    saveHostname = g_Devs(idx).sHostname
    saveSmpRate  = g_Devs(idx).dwSmpRate

    LeaveCriticalSection g_csDev

    ' AddLog OUTSIDE CS - never hold CS during UI operations
    IF bNew THEN
        LOCAL sDevName AS STRING
        IF LEN(TRIM$(saveHostname)) > 0 THEN
            sDevName = TRIM$(saveHostname)
        ELSE
            sDevName = sMac
        END IF
        AddLog "New device: " & sDevName & " MAC=" & sMac & " IP=" & FormatIP(dwIP) & ":" & TRIM$(STR$(dwPort)) & _
               " rate=" & TRIM$(STR$(saveSmpRate)) & " Hz"
    END IF

    ' FIX (BUG#1+AUTOTRANSPORT): if the ESP switched UDP<->TCP while the
    ' stream was active, restart ONLY this device's audio thread - not
    ' every checked device. Previously this called StopCheckedStreams +
    ' StartCheckedStreams, which stopped and restarted ALL checked streams
    ' just because one changed transport. With multiple devices streaming,
    ' a single ESP's AT+XPORT + HOTRESTART would drop audio on all of them
    ' for ~3s. Now uses per-device StopStream + StartStream.
    '
    ' NOTE: if the user unchecked this device in the ListView while we
    ' were processing, StartStream will see dwHBActive=0 still set by
    ' StopStream and... actually StartStream checks dwActive, not the
    ' checkbox. We respect dwHBActive=0 from StopStream: StartStream's
    ' guard "dwHBActive <> 0" will fail (it's 0 after stop), so we DO
    ' restart. This is correct - the device was streaming (bTransportChanged
    ' only fires when dwHBActive<>0), so the user wants it back.
    IF bTransportChanged THEN
        LOCAL sOldTr AS STRING, sNewTr AS STRING
        LOCAL stopResult AS LONG
        IF prevTransport = 0 THEN sOldTr = "UDP" ELSE sOldTr = "TCP"
        IF g_Devs(idx).dwTransport = 0 THEN sNewTr = "UDP" ELSE sNewTr = "TCP"
        AddLog "[AUTOTRANSPORT] dev #" & TRIM$(STR$(idx)) & _
               ": ESP switched " & sOldTr & " -> " & sNewTr & _
               ", auto-restarting audio thread (per-device)"
        ' FIX (grok22#2): check StopStream return. If it timed out (thread
        ' still alive after 30s), do NOT call StartStream - that would
        ' create a duplicate AudioThread on the same port.
        ' FIX (AUDIT-W2-FIX): if StopStream timed out (returned 0), it left
        ' an orphan handle in g_Devs. Call StopStream AGAIN - the second
        ' call hits the orphan-reaping block (dwHBActive=0 AND
        ' hAudioThread<>0), waits briefly for the now-self-exited orphan,
        ' closes the handle and returns 1. Then StartStream can safely
        ' proceed. This restores AUTOTRANSPORT resilience: the device
        ' auto-restarts on UDP<->TCP switch even if the first stop timed out.
        stopResult = StopStream(idx)
        IF stopResult = 0 THEN
            stopResult = StopStream(idx)   ' reap orphan handle, returns 1
        END IF
        IF stopResult = 0 THEN
            AddLog "[AUTOTRANSPORT] dev #" & TRIM$(STR$(idx)) & _
                   ": StopStream timed out twice - NOT restarting (manual restart needed)"
        ELSE
            SLEEP 200   ' brief pause for OS socket recycling
            StartStream idx
        END IF
    END IF
END SUB

' SendDiscover - Send DISCOVER command (heartbeat)
SUB SendDiscover(BYVAL targetIP AS DWORD, BYVAL targetPort AS DWORD)
    LOCAL sndBuf AS STRING
    LOCAL seq AS LONG
    LOCAL sndErr AS LONG

    sndBuf = CHR$(%EASSP_MAGIC0, %EASSP_MAGIC1, %EASSP_VER, %CMD_DISCOVER)
    seq = InterlockedIncrement(g_seqCnt)  ' FIX (H19): capture return atomically
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
    seq = InterlockedIncrement(g_seqCnt)  ' FIX (H19): capture return atomically
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
    seq = InterlockedIncrement(g_seqCnt)  ' FIX (H19): capture return atomically
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
    LOCAL hbPktRecv AS DWORD
    LOCAL hbIter AS LONG          ' DIAG: iteration counter
    LOCAL hbSentThisIter AS LONG  ' DIAG: how many DISCOVER sent this iteration

    ' FIX (BUG#6): hoist these DIMs OUT of the DO WHILE loop. Previously
    ' they were re-DIM'd inside the loop every %HB_INTERVAL (3s), which
    ' re-allocates the arrays on each iteration - the STRING array
    ' (rsReasons) is especially expensive: each iteration allocated 16
    ' PBSTR descriptors + BSTR heap, only to be immediately overwritten.
    ' The arrays persist between iterations, but the count variables are
    ' explicitly reset to 0 inside the loop, so semantics are unchanged.
    DIM hbTargets(0 TO %MAX_DEVICES - 1) AS LOCAL DWORD   ' IP
    DIM hbPorts(0 TO %MAX_DEVICES - 1) AS LOCAL DWORD    ' Port
    DIM hbCount AS LOCAL LONG
    DIM rsIPs(0 TO %MAX_DEVICES - 1) AS LOCAL DWORD
    DIM rsPorts(0 TO %MAX_DEVICES - 1) AS LOCAL DWORD
    DIM rsAudioPorts(0 TO %MAX_DEVICES - 1) AS LOCAL DWORD
    DIM rsIdxs(0 TO %MAX_DEVICES - 1) AS LOCAL LONG
    DIM rsReasons(0 TO %MAX_DEVICES - 1) AS LOCAL STRING
    DIM rsCount AS LOCAL LONG

    AddLog "[HB] HeartbeatThread started (interval=" & TRIM$(STR$(%HB_INTERVAL)) & "ms)"

    DO WHILE g_bRunning
        SLEEP %HB_INTERVAL
        IF g_bRunning = 0 THEN EXIT DO

        INCR hbIter
        hbSentThisIter = 0

        ' FIX H2: Collect (IP,Port) pairs under CS, then send OUTSIDE CS.
        ' Previously SendDiscover was called inside CS → UDP SEND could block
        ' → GUI thread (RefreshUI/UpdateButtonStates) waiting on CS → UI freeze.
        hbCount = 0

        ' FIX (WiFi-drop recovery): collect ALL devices needing a CONFIGURE
        ' resend, not just one. Two stall cases:
        '   (a) dwPktRecv = 0  → initial CONFIGURE was lost (startup), 3s
        '   (b) dwPktRecv > 0  → stream was running then packets stopped
        '       (WiFi AP dropped; ESP watchdog-timed-out to IDLE after 15s;
        '        ESP only resumes on a new CONFIGURE). %STREAM_STALL_MS.
        ' The old code only handled (a) and only one device per iteration,
        ' so a mid-flight WiFi drop left the server stuck in "Streaming"
        ' forever while the ESP sat IDLE waiting for CONFIGURE.
        rsCount = 0

        EnterCriticalSection g_csDev
        tick = GetTickCount()

        FOR idx = 0 TO %MAX_DEVICES - 1
            IF g_Devs(idx).dwActive = 0 THEN ITERATE FOR

            IF g_Devs(idx).dwHBActive THEN
                ' Collect target for DISCOVER heartbeat OUTSIDE CS
                hbTargets(hbCount) = g_Devs(idx).dwIP
                hbPorts(hbCount)   = g_Devs(idx).dwPort
                INCR hbCount

                ' ---- Stall detection: decide if this device needs a CONFIGURE resend ----
                hbPktRecv = g_Devs(idx).dwPktRecv
                IF hbPktRecv = 0 THEN
                    ' Case (a): never received any audio since stream start.
                    ' Initial CONFIGURE was probably lost in WiFi.
                    elapsed = tick - g_Devs(idx).dwStreamStart
                    IF elapsed > %NO_AUDIO_RESEND_MS THEN
                    ' FIX (BUG#12): was hardcoded 'elapsed > 3000'. Startup skip
                    ' (1s) + fade-in (1s) + socket open + prebuffer can exceed
                    ' 3s on slow WiFi / cold ESP boot, causing spurious CONFIGURE
                    ' resends during normal startup. NO_AUDIO_RESEND_MS (5s in
                    ' config.inc) only fires on real 'initial CONFIGURE lost' cases.
                        rsIPs(rsCount)        = g_Devs(idx).dwIP
                        rsPorts(rsCount)      = g_Devs(idx).dwPort
                        rsAudioPorts(rsCount) = g_Devs(idx).dwAudioPort
                        rsIdxs(rsCount)       = idx
                        rsReasons(rsCount)    = "no audio " & TRIM$(STR$(elapsed)) & _
                                                "ms after start (initial CONFIGURE lost?)"
                        INCR rsCount
                    END IF
                ELSE
                    ' Case (b): stream was running, then packets stopped.
                    ' dwLastPktTick is updated on every received audio packet
                    ' (AudioThread stats block). If it hasn't moved for
                    ' %STREAM_STALL_MS, the ESP is either unreachable (WiFi
                    ' still down) or has watchdog-timed-out to IDLE. Resend
                    ' CONFIGURE so that whenever the ESP comes back online it
                    ' resumes streaming. Harmless if ESP is still STREAMING
                    ' (same-dest CONFIGURE just resets its watchdog).
                    elapsed = tick - g_Devs(idx).dwLastPktTick
                    IF elapsed > %STREAM_STALL_MS THEN
                        rsIPs(rsCount)        = g_Devs(idx).dwIP
                        rsPorts(rsCount)      = g_Devs(idx).dwPort
                        rsAudioPorts(rsCount) = g_Devs(idx).dwAudioPort
                        rsIdxs(rsCount)       = idx
                        rsReasons(rsCount)    = "stream stalled - no audio for " & _
                                                TRIM$(STR$(elapsed)) & _
                                                "ms (WiFi drop? resending CONFIGURE)"
                        INCR rsCount
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

        ' DIAG: log every heartbeat iteration when streaming, every 5th when idle.
        ' FIX (GROK-17): the streaming branch fires every 3s -> log spam in
        ' production. Gate it behind g_bVerbose (default 0 = suppressed).
        ' The idle branch (every 5th iter = 15s) always logs so the user
        ' can still see the server is alive when no streams are active.
        IF hbSentThisIter > 0 THEN
            IF g_bVerbose THEN
                AddLog "[HB] iter=" & TRIM$(STR$(hbIter)) & _
                       " sent " & TRIM$(STR$(hbSentThisIter)) & " DISCOVER"
            END IF
        ELSEIF (hbIter MOD 5) = 0 THEN
            AddLog "[HB] iter=" & TRIM$(STR$(hbIter)) & " idle (no HBActive devices)"
        END IF

        ' Resend CONFIGURE to ALL stalled devices OUTSIDE CS (UDP SEND can block)
        FOR idx = 0 TO rsCount - 1
            AddLog "[HB] dev #" & TRIM$(STR$(rsIdxs(idx))) & ": " & rsReasons(idx)
            SendConfigure rsIPs(idx), rsPorts(idx), rsAudioPorts(idx)
        NEXT idx
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

' EspActualRate - Вычисляет ФАКТИЧЕСКУЮ частоту I2S ESP8266 для данного
' номинала и бит.глубины I2S. Зеркалит i2s_set_rate() из firmware/i2s.c:
'   160 МГц / (bits×2ch) / (bck_div × mclk_div), оба делителя целые 1..63.
'
' ЗАЧЕМ: ESP не может выдать ровно 44100 Гц — реально 43860 Гц (-0.545%).
' Если открыть WaveOut на номинале 44100, буфер опустошается быстрее, чем
' ESP наполняет → underrun → пощипывание/щелчки в LIVE режиме (дамп при
' этом чистый, т.к. пишет все пакеты синхронно).
'
' Открываем WaveOut на фактической частоте ESP → дрейф = 0 → финальный
' ресамплинг до частоты карты делает Windows-аудиодвижок (качественно).
'
' Параметры:
'   nominalHz - номинал из INFO-пакета (8000/11025/.../48000)
'   i2sBits   - РЕАЛЬНАЯ бит.глубина I2S (devBits из INFO, 16 или 24).
'               НЕ outBits! Для ADPCM outBits=16, но I2S может быть 24.
FUNCTION EspActualRate(BYVAL nominalHz AS DWORD, BYVAL i2sBits AS LONG) AS DWORD
    LOCAL scaled AS DOUBLE
    LOCAL i AS LONG, j AS LONG
    LOCAL bestI AS LONG, bestJ AS LONG
    LOCAL bestDelta AS DOUBLE, d AS DOUBLE, actual AS DOUBLE

    ' 160 МГц / (bits × 2ch)
    IF i2sBits = 24 THEN
        scaled = 160000000.0# / 48.0#     ' 3 333 333.33
    ELSE
        scaled = 160000000.0# / 32.0#     ' 5 000 000
    END IF

    ' Перебор всех пар делителей 1..63 (как в прошивке)
    bestI = 1
    bestJ = 1
    bestDelta = scaled                  ' больше любого возможного delta
    FOR i = 1 TO 63
        FOR j = i TO 63                 ' j >= i: симметрично, отсекаем дубли
            actual = scaled / (CSNG(i) * CSNG(j))
            d = ABS(actual - nominalHz)
            IF d < bestDelta THEN
                bestDelta = d
                bestI = i
                bestJ = j
                IF d = 0! THEN EXIT FOR ' точное попадание
            END IF
        NEXT j
        IF bestDelta = 0! THEN EXIT FOR
    NEXT i

    actual = scaled / (CSNG(bestI) * CSNG(bestJ))
    FUNCTION = CLNG(actual)
END FUNCTION

' CodecName - Return human-readable codec name from codec ID.
FUNCTION CodecName(BYVAL codecId AS LONG) AS STRING
    SELECT CASE codecId
        CASE %CODEC_ID:     FUNCTION = "ADPCM"
        CASE %CODEC_ID_PCM: FUNCTION = "PCM"
        CASE ELSE:          FUNCTION = "Unknown(" & TRIM$(STR$(codecId)) & ")"
    END SELECT
END FUNCTION

' FIX (GROK-4): MACRO that resets ALL audio-pipeline local state after a
' TCP reconnect / HOTRESTART / sleep-wake. The 4 reconnect paths previously
' had asymmetric resets (paths 1,2 did full re-arm; paths 3,4 only partial;
' NONE reset pktRecv). This caused an OOO-silence-forever bug: after any
' reconnect, pktRecv > 0 while lastSeq = 0 -> on the next packet,
' seqDiff = (seqNum - 0 - 1) AND 0xFFFF. If seqNum >= 32768, oooPkt = 1
' (false OOO) -> packet discarded -> dwLastPktTick updated -> HB stall
' detection never fires -> silence until manual restart. The HOTRESTART
' heuristic at line ~2351 doesn't catch this because lastSeq was reset to
' 0 (so lastSeq > 1000 is FALSE). This macro unifies all 5 reset paths.
' Must be called from inside AudioThread (references its LOCALs).
MACRO ResetPipelineState()
    ' ADPCM decoder state (per-channel predictors)
    predictor  = 0
    stepIndex  = 0
    predictor2 = 0
    stepIndex2 = 0
    ' Jitter buffer: force re-prebuffer from scratch
    jitterPrebuffering = 1
    jitterFilled = 0
    ' Startup skip + fade-in (1s skip + 1s fade, same as cold start)
    skipFramesRemaining = 999
    fadeSamplesRemaining = 0
    ' PLC state
    plcActive = 0
    lastPcmPtr = 0
    lastPcmLen = 0
    ' FIX (GROK-4): CRITICAL - reset pktRecv so the OOO detection on the
    ' next packet doesn't misfire. With pktRecv > 0 and lastSeq = 0,
    ' seqDiff = (seqNum - 0 - 1) AND 0xFFFF would be > 32768 for any
    ' seqNum > 32768 -> false OOO -> packet discarded -> silence forever.
    pktRecv = 0
    lastSeq = 0
    lostPkt = 0
    oooPkt  = 0
    ' Reset per-device stats + stall-detection baseline under CS so
    ' HB stall detection treats this as a fresh start.
    EnterCriticalSection g_csDev
    g_Devs(idx).dwPktRecv     = 0
    g_Devs(idx).dwPktOOO      = 0
    g_Devs(idx).dwPktLost     = 0
    g_Devs(idx).wLastSeq      = 0
    g_Devs(idx).dwStreamStart = GetTickCount()
    g_Devs(idx).dwLastPktTick = g_Devs(idx).dwStreamStart
    LeaveCriticalSection g_csDev
END MACRO

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
    LOCAL devTransport AS LONG
    devTransport = g_Devs(idx).dwTransport
    LOCAL devIP AS DWORD
    devIP = g_Devs(idx).dwIP
    ' FIX (GROK-8): snapshot dwWaveDevice under CS too. Previously this
    ' field was read directly from g_Devs(idx).dwWaveDevice in all 6
    ' waveOutOpen call sites (initial open + sleep-wake reopen + format-
    ' change reopen), inconsistent with the CS-protected write side
    ' (SetDeviceWaveOutput at line ~605). On x86-32 a DWORD LONG write is
    ' atomic so practical impact was limited (worst case: stale device ID
    ' for one call), but this closes the consistency hole. The local
    ' devWaveDevice is re-read on each reopen path below to pick up
    ' mid-stream device changes (SetDeviceWaveOutput also sets
    ' bReopenWaveOut=1).
    LOCAL devWaveDevice AS LONG
    devWaveDevice = g_Devs(idx).dwWaveDevice
    ' curRateEnum tracks the sample-rate enum (0..6) currently open in WaveOut.
    ' Initialized from devSmpRate via Hz-to-enum conversion; updated on format change.
    LOCAL curRateEnum AS LONG
    curRateEnum = HzToRateEnum(devSmpRate)
    LeaveCriticalSection g_csDev

    IF nCh < 1 THEN nCh = 1
    IF nCh > 2 THEN nCh = 2
    IF devSmpRate < 8000 OR devSmpRate > 48000 THEN devSmpRate = 48000
    IF devBits <> 16 AND devBits <> 24 THEN devBits = 16
    ' HARDENING (defense in depth): ProcessInfo already corrects dwTransport,
    ' but re-validate here in case state was corrupted between INFO and start.
    ' Only 0 (UDP) and 1 (TCP) are receivable. Raw (2) or garbage -> UDP.
    IF devTransport <> 0 AND devTransport <> 1 THEN
        AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
               ": invalid transport=" & TRIM$(STR$(devTransport)) & _
               " -> forcing UDP (0)"
        devTransport = 0
    END IF

    ' Open transport: TCP connect to ESP (if transport=1) or UDP listen (default).
    fNum = FREEFILE
    ERRCLEAR
    IF devTransport = 1 THEN
        ' TCP mode: ESP = listener, we connect to ESP_IP:bindPort.
        ' Retry connect every 1s until success or stream stops. ESP may
        ' not have opened its listening socket yet (CONFIGURE → stream
        ' start → tcp_stream_init_listen is async on ESP side).
        LOCAL tcpHost AS STRING
        tcpHost = FormatIP(devIP)
        LOCAL connectAttempts AS LONG
        connectAttempts = 0
        AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
               ": TCP connecting to " & tcpHost & ":" & TRIM$(STR$(bindPort))
        ' FIX (M40): removed dead DO WHILE loop (it had an unconditional
        ' EXIT DO on the first iteration, so it never looped). The loop
        ' body also read dwRunning/dwActive without CS - a pointless race.
        DO
            ERRCLEAR
            TCP OPEN PORT bindPort AT tcpHost AS #fNum TIMEOUT 2000
            IF ERR = 0 THEN EXIT DO
            INCR connectAttempts
            IF connectAttempts = 1 OR (connectAttempts MOD 10) = 0 THEN
                AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                       ": TCP connect attempt " & TRIM$(STR$(connectAttempts)) & _
                       " failed err=" & TRIM$(STR$(ERR)) & ", retrying..."
            END IF
            ' Check if we should stop (parent cleared dwRunning externally)
            IF g_Devs(idx).dwRunning = 0 AND connectAttempts > 1 THEN
                ' Stream was stopped while we were trying to connect
                AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & ": stream stopped during TCP connect"
                FUNCTION = 0
                EXIT FUNCTION
            END IF
            SLEEP 1000
        LOOP
        AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
               ": TCP connected to " & tcpHost & ":" & TRIM$(STR$(bindPort)) & _
               " (file #" & TRIM$(STR$(fNum)) & ")"
    ELSE
        ' UDP mode (default): open listening UDP socket on bindPort.
        ' FIX (UDP stop crash): short RECV timeout (500ms) so AudioThread polls
        ' dwRunning frequently. With the old 60s timeout the thread could be
        ' blocked deep inside UDP RECV when the main thread signalled stop;
        ' closing #fNum from the main thread while RECV is in progress races
        ' on PowerBASIC's process-global file-number table (CLOSE frees the
        ' slot while RECV still references it -> use-after-free -> crash).
        ' Now the thread notices dwRunning=0 within 500ms and closes its OWN
        ' socket in the cleanup section.
        UDP OPEN PORT bindPort AS #fNum TIMEOUT 500
        IF ERR THEN
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                   ": UDP OPEN PORT " & TRIM$(STR$(bindPort)) & " FAILED err=" & TRIM$(STR$(ERR))
            EnterCriticalSection g_csDev
            g_Devs(idx).fAudioFile = 0
            g_Devs(idx).dwRunning  = 0
            LeaveCriticalSection g_csDev
            FUNCTION = 0
            EXIT FUNCTION
        END IF
        AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
               ": UDP OPEN PORT " & TRIM$(STR$(bindPort)) & " OK (file #" & TRIM$(STR$(fNum)) & ")"
    END IF

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
            ' FIX (AUDIT-H9): branch on devTransport so we don't
            ' call UDP CLOSE on a TCP socket (silently leaks the
            ' TCP fd in TCP mode).
            IF devTransport = 1 THEN TCP CLOSE #fNum ELSE UDP CLOSE #fNum
            EnterCriticalSection g_csDev
            ' FIX (AUDIT-H10): clear dwHBActive + dwStatus too so the
            ' UI does not stay in "Streaming" after a failed start.
            g_Devs(idx).fAudioFile = 0
            g_Devs(idx).dwRunning  = 0
            g_Devs(idx).dwHBActive = 0
            g_Devs(idx).dwStatus   = %STS_IDLE
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

    ' <<<DRIFT FIX>>> открываем WaveOut на ФАКТИЧЕСКОЙ частоте I2S ESP,
    ' иначе при 44100 Гц (реально 43860) буфер опустошается быстрее →
    ' underrun → пощипывание в LIVE (дамп чистый, т.к. пишет синхронно).
    ' Передаём devBits (реальная бит.глубина I2S), НЕ outBits.
    LOCAL actualSmpRate AS DWORD
    actualSmpRate = EspActualRate(devSmpRate, devBits)
    IF actualSmpRate <> devSmpRate THEN
        AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
               ": rate trim " & TRIM$(STR$(devSmpRate)) & "Hz -> " & _
               TRIM$(STR$(actualSmpRate)) & "Hz (ESP I2S clock, drift fix)"
    END IF

    wfFmt.wFormatTag      = %WAVE_FORMAT_PCM
    wfFmt.nChannels       = nCh
    wfFmt.nSamplesPerSec  = actualSmpRate
    wfFmt.wBitsPerSample  = outBits
    wfFmt.nBlockAlign     = nCh * (outBits \ 8)
    wfFmt.nAvgBytesPerSec = wfFmt.nSamplesPerSec * wfFmt.nBlockAlign
    wfFmt.cbSize          = 0

    ' Open WaveOut. Try 24-bit first if ESP is 24-bit; if the card rejects
    ' it (WAVERR_BADFORMAT or MMSYSERR), retry with 16-bit and mark for
    ' on-the-fly 24->16 conversion.
    LOCAL waveResult AS LONG
    ' FIX (GROK-8): use snapshotted devWaveDevice instead of direct g_Devs read.
    waveResult = waveOutOpen(waveOut, devWaveDevice, wfFmt, 0, 0, %CALLBACK_NULL)
    IF waveResult <> 0 AND outBits = 24 THEN
        AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
               ": 24-bit WaveOut failed (err=" & TRIM$(STR$(waveResult)) & _
               "), falling back to 16-bit"
        outBits = 16
        wfFmt.wBitsPerSample  = 16
        wfFmt.nBlockAlign     = nCh * 2
        wfFmt.nAvgBytesPerSec = wfFmt.nSamplesPerSec * wfFmt.nBlockAlign
        waveResult = waveOutOpen(waveOut, devWaveDevice, wfFmt, 0, 0, %CALLBACK_NULL)
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
        ' FIX (AUDIT-H9): branch on devTransport so we don't
        ' call UDP CLOSE on a TCP socket (silently leaks the
        ' TCP fd in TCP mode).
        IF devTransport = 1 THEN TCP CLOSE #fNum ELSE UDP CLOSE #fNum
        EnterCriticalSection g_csDev
        ' NOTE: Do NOT clear hAudioThread - main thread owns handle cleanup
        ' FIX (AUDIT-H10): clear dwHBActive + dwStatus too so the UI
        ' does not stay in "Streaming" after a failed waveOutOpen.
        g_Devs(idx).dwRunning  = 0
        g_Devs(idx).fAudioFile = 0
        g_Devs(idx).dwHBActive = 0
        g_Devs(idx).dwStatus   = %STS_IDLE
        LeaveCriticalSection g_csDev
        FUNCTION = 0
        EXIT FUNCTION
    END IF

    EnterCriticalSection g_csDev
    g_Devs(idx).hWaveOut = waveOut
    LeaveCriticalSection g_csDev

    ' Prepare headers
    ' FIX (BUG#15): check waveOutPrepareHeader return. A failed prepare
    ' leaves the WAVEHDR in an inconsistent state - subsequent waveOutWrite
    ' silently fails or plays garbage. Log + skip the buffer (mark INQUEUE
    ' so the submit scan won't pick it).
    LOCAL prepRes AS LONG
    FOR wIdx2 = 0 TO %NUM_WAVE_BUFS - 1
        prepRes = waveOutPrepareHeader(waveOut, whdrs(wIdx2), SIZEOF(WAVEHDR))
        IF prepRes <> 0 THEN
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                   ": waveOutPrepareHeader FAILED buf=" & TRIM$(STR$(wIdx2)) & _
                   " err=" & TRIM$(STR$(prepRes)) & " - buffer disabled"
            ' Mark as INQUEUE so submit scan skips it; otherwise we'd send
            ' an unprepared header to waveOutWrite -> UB.
            whdrs(wIdx2).dwFlags = whdrs(wIdx2).dwFlags OR %WHDR_INQUEUE
        END IF
    NEXT wIdx2

    ' Init stats under CS
    ' FIX (GROK-1): dwRunning is now set in StartStream BEFORE THREAD CREATE,
    ' so the TCP connect loop's abort condition (dwRunning=0 AND attempts>1)
    ' works correctly. We keep the assignment here too as a defensive
    ' re-affirmation in case StartStream's set was lost (e.g. thread
    ' scheduling delayed the read); it's idempotent.
    EnterCriticalSection g_csDev
    g_Devs(idx).dwRunning     = 1
    g_Devs(idx).dwPktRecv     = 0
    g_Devs(idx).dwPktOOO      = 0
    g_Devs(idx).dwPktLost     = 0
    g_Devs(idx).dwBytesRecv   = 0
    g_Devs(idx).wLastSeq      = 0
    g_Devs(idx).dwStreamStart = GetTickCount()
    g_Devs(idx).dwLastPktTick = g_Devs(idx).dwStreamStart   ' stall-detection baseline
    LeaveCriticalSection g_csDev

    predictor  = 0
    stepIndex  = 0
    predictor2 = 0
    stepIndex2 = 0
    lastSeq = 0
    pktRecv = 0
    wIdx = 0

    ' ---- Jitter Buffer / PLC / Adaptive state ----
    LOCAL jitterTarget AS LONG     ' Current prebuffer target (adaptive)
    LOCAL jitterFilled AS LONG     ' Frames accumulated in prebuffer phase
    LOCAL jitterPrebuffering AS LONG  ' 1 = still prebuffering, 0 = playing
    LOCAL underrunCount AS LONG    ' Underruns in current adaptation interval
    ' FIX (GROK-5): separate counter for overflow events (queue full).
    ' Previously overflow used underrunCount, mislabeling the event and
    ' feeding it into the adaptive-grow decision (which would grow the
    ' buffer when it was already full). Now overflows only increment this
    ' counter; the adaptive logic can use it to SHRINK jitterTarget.
    LOCAL overflowCount AS LONG    ' Overflows in current adaptation interval
    LOCAL lastAdaptTick AS DWORD   ' Last adaptive adjustment time
    ' FIX (log-fix-C): consecutive zero-underrun intervals (need 2 to decrease)
    LOCAL zeroUnderrunIntervals AS LONG
    LOCAL lastPcmPtr AS DWORD      ' Pointer to last decoded PCM (for PLC)
    LOCAL lastPcmLen AS LONG       ' Length of last decoded PCM (for PLC)
    ' FIX (grok22#5): per-channel last sample for stereo PLC ramp.
    ' Previously a single lastPcmLastSample held only L's last value,
    ' so R was ramped using L's interpolation -> discontinuity on R.
    ' Array index 0=L, 1=R (unused for mono).
    ' FIX (compile): PowerBASIC requires DIM (not LOCAL) for local arrays.
    DIM lastPcmLastSample(1) AS LOCAL LONG  ' Last sample value per channel

    LOCAL plcActive AS LONG        ' 1 = PLC interpolation in progress

    ' FIX (startup clicks): skip the first ~1 second of audio after stream
    ' start. The INMP441 I2S MEMS mic has a startup transient (DC offset
    ' settling + membrane ringing) that lasts ~10-15ms, plus the I2S DMA
    ' buffers contain stale data from the previous stream. Skipping the first
    ' second of received audio ensures the mic is fully stable and all stale
    ' DMA data is drained before we start playing. The skip happens AFTER
    ' receive (so packets are consumed, keeping the socket drained) but BEFORE
    ' decode/playback (so the transient never reaches WaveOut).
    ' The actual frame count is computed from frame_ms (parsed from the first
    ' packet header, offset 9). Initialized to a large value; corrected after
    ' the first packet arrives.
    LOCAL skipFramesRemaining AS LONG
    LOCAL skipFrameMs AS LONG
    skipFramesRemaining = 999   ' large sentinel; replaced after first packet
    skipFrameMs = 20            ' default; updated from packet header

    ' FIX (startup clicks): after the skip completes, apply a linear fade-in
    ' over FADE_IN_MS (1 second) to smooth the silence→signal transition.
    ' Even after 1 second of skip, the first played sample may have a small
    ' DC step relative to the digital silence in WaveOut buffers — fade-in
    ' ramps amplitude from 0 to 1 over 1 second, making the onset inaudible.
    ' fadeSamplesRemaining = total samples in FADE_IN_MS, computed from the
    ' actual sample rate (from the first packet). 0 = fade-in complete.
    LOCAL fadeSamplesRemaining AS LONG
    LOCAL fadeSamplesTotal AS LONG
    fadeSamplesRemaining = 0   ' armed when skip completes
    fadeSamplesTotal = 0

    jitterTarget = %JITTER_INITIAL
    jitterFilled = 0
    jitterPrebuffering = 1
    underrunCount = 0
    overflowCount = 0
    lastAdaptTick = GetTickCount()
    lastPcmPtr = 0
    lastPcmLen = 0
    lastPcmLastSample(0) = 0
    lastPcmLastSample(1) = 0
    plcActive = 0

    AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
           ": jitter buffer: prebuffer=" & TRIM$(STR$(jitterTarget)) & " frames"

    ' ---- Main audio loop ----
    ' TCP framing: [u16 length BE][16-byte pkt_header][payload].
    ' UDP: each datagram = [16-byte pkt_header][payload] (boundaries preserved).
    ' After the receive block, recvBuf = [header][payload] in BOTH cases,
    ' so the parsing code below is transport-agnostic.
    LOCAL audRecvCount AS LONG    ' DIAG: count of successful RECV
    LOCAL audErrCount  AS LONG    ' DIAG: count of ERR on RECV
    audRecvCount = 0
    audErrCount  = 0

    ' TCP framing state
    LOCAL tcpLenBuf AS STRING        ' 2-byte length prefix buffer
    LOCAL tcpPktLen AS LONG          ' decoded packet length (16..1416)
    LOCAL tcpGot AS LONG             ' bytes received so far in current frame
    LOCAL tcpNeed AS LONG            ' bytes still needed
    LOCAL tcpTmp AS STRING           ' temp buffer for partial reads
    LOCAL tcpReconnectCount AS LONG
    tcpReconnectCount = 0

    DO WHILE g_Devs(idx).dwRunning
        ERRCLEAR

        IF devTransport = 1 THEN
            ' ---- TCP framing read ----
            ' 1) Read 2-byte length prefix (big-endian).
            ' PowerBASIC TCP RECV may return fewer bytes than requested,
            ' so loop until we have exactly 2 bytes (or error/timeout).
            tcpLenBuf = ""
            DO WHILE LEN(tcpLenBuf) < 2
                ERRCLEAR
                LOCAL tmpLen AS STRING
                TCP RECV #fNum, 2 - LEN(tcpLenBuf), tmpLen
                IF ERR OR LEN(tmpLen) = 0 THEN EXIT DO
                tcpLenBuf = tcpLenBuf & tmpLen
            LOOP
            IF ERR OR LEN(tcpLenBuf) < 2 THEN
                ' Connection broken / closed by ESP. Reconnect.
                INCR audErrCount
                IF audErrCount = 1 OR (audErrCount MOD 30) = 0 THEN
                    AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                           ": TCP RECV len-prefix err=" & TRIM$(STR$(ERR)) & _
                           " got=" & TRIM$(STR$(LEN(tcpLenBuf))) & _
                           " (count=" & TRIM$(STR$(audErrCount)) & ")"
                END IF
                ' Close + retry connect (ESP may have restarted stream).
                TCP CLOSE #fNum
                ERRCLEAR
                INCR tcpReconnectCount
                AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & ": TCP reconnecting to " & tcpHost & ":" & TRIM$(STR$(bindPort)) & " (attempt " & TRIM$(STR$(tcpReconnectCount)) & ")"
                LOCAL reconOk AS LONG
                LOCAL reconTries AS LONG
                reconOk = 0
                reconTries = 0
                DO WHILE g_Devs(idx).dwRunning
                    ERRCLEAR
                    TCP OPEN PORT bindPort AT tcpHost AS #fNum TIMEOUT 2000
                    IF ERR = 0 THEN reconOk = 1 : EXIT DO
                    INCR reconTries
                    ' Log every 5th failed attempt to show we're still trying
                    IF (reconTries MOD 5) = 1 THEN
                        AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                               ": TCP connect failed (try " & TRIM$(STR$(reconTries)) & _
                               ", err=" & TRIM$(STR$(ERR)) & ") - retrying"
                    END IF
                    SLEEP 1000
                LOOP
                IF reconOk = 0 THEN EXIT DO   ' stream stopped
                AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                       ": TCP reconnected (attempt " & TRIM$(STR$(tcpReconnectCount)) & ")"
                ' FIX (GROK-4): unified pipeline reset via macro. Previously
                ' this path reset jitter/predictor/skip/fade/lastPcm but NOT
                ' pktRecv - causing false OOO detection on the next packet
                ' when seqNum > 32768 -> silence forever. The macro now
                ' resets ALL state including pktRecv + per-device stats.
                ResetPipelineState
                ITERATE DO
            END IF
            ' Decode big-endian u16 length.
            tcpPktLen = (ASC(tcpLenBuf, 1) * 256&) + ASC(tcpLenBuf, 2)
            IF tcpPktLen < %PKT_HDR_SZ OR tcpPktLen > 1416 THEN
                ' Invalid frame — resync by closing+reconnecting (rare).
                AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                       ": TCP invalid frame len=" & TRIM$(STR$(tcpPktLen)) & ", resyncing"
                TCP CLOSE #fNum
                ERRCLEAR
                SLEEP 500
                DO WHILE g_Devs(idx).dwRunning
                    ERRCLEAR
                    TCP OPEN PORT bindPort AT tcpHost AS #fNum TIMEOUT 2000
                    IF ERR = 0 THEN EXIT DO
                    SLEEP 1000
                LOOP
                ' FIX (GROK-4): unified pipeline reset via macro (same as
                ' len-prefix-error path above). Previously this path reset
                ' state partially and did NOT reset pktRecv -> false OOO
                ' on seqNum > 32768 -> silence forever.
                ResetPipelineState
                ITERATE DO
            END IF
            ' 2) Read tcpPktLen bytes (payload = pkt_header + data).
            ' FIX (M39): pre-allocate recvBuf to the expected size and POKE$
            ' into it. The previous code used string concatenation in the
            ' loop (recvBuf = recvBuf & tcpTmp), which reallocates + copies
            ' the entire string on every iteration. With 1-byte reads (high
            ' fragmentation on WiFi), reading a 1400-byte frame is O(n^2).
            ' FIX (AUDIT-C1): update tcpNeed BEFORE allocating recvBuf.
            ' The previous order (alloc then update) allocated the buffer
            ' with the STALE tcpNeed (0 on the first frame, previous-frame
            ' size on subsequent frames). The POKE$ below then wrote
            ' tcpPktLen bytes into a smaller buffer -> heap corruption.
            tcpNeed = tcpPktLen
            recvBuf = SPACE$(tcpNeed)
            tcpGot = 0
            DO WHILE tcpGot < tcpNeed
                ERRCLEAR
                TCP RECV #fNum, (tcpNeed - tcpGot), tcpTmp
                IF ERR OR LEN(tcpTmp) = 0 THEN
                    ' Partial frame - connection broke mid-read. Reconnect.
                    EXIT DO
                END IF
                POKE$ STRPTR(recvBuf) + tcpGot, tcpTmp
                tcpGot = tcpGot + LEN(tcpTmp)
            LOOP
            IF tcpGot < tcpNeed THEN recvBuf = LEFT$(recvBuf, tcpGot)
            IF tcpGot < tcpNeed THEN
                ' Reconnect on partial read.
                INCR audErrCount
                TCP CLOSE #fNum
                ERRCLEAR
                DO WHILE g_Devs(idx).dwRunning
                    ERRCLEAR
                    TCP OPEN PORT bindPort AT tcpHost AS #fNum TIMEOUT 2000
                    IF ERR = 0 THEN EXIT DO
                    SLEEP 1000
                LOOP
                ' FIX (GROK-4): unified pipeline reset via macro. Previously
                ' this path reset only predictor/jitter/lastSeq but NOT
                ' skip/fade/lastPcm/pktRecv -> false OOO on seqNum > 32768
                ' -> silence forever. The macro now resets ALL state.
                ResetPipelineState
                ITERATE DO
            END IF
            ' recvBuf now = [16-byte header][payload], same as UDP path.
            ' fromIP/fromPort unused for TCP (single connection), set for safety.
            fromIP = devIP
            fromPort = bindPort
        ELSE
            ' ---- UDP datagram receive (original path) ----
            UDP RECV #fNum, FROM fromIP, fromPort, recvBuf
            IF ERR THEN
                INCR audErrCount
                IF audErrCount = 1 OR (audErrCount MOD 5000) = 0 THEN
                    AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                           ": UDP RECV err=" & TRIM$(STR$(ERR)) & _
                           " (count=" & TRIM$(STR$(audErrCount)) & ") - no data?"
                END IF
                SLEEP 2
                ITERATE DO
            END IF
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
                   " (expect " & TRIM$(STR$(%CODEC_ID)) & " or " & _
                   TRIM$(STR$(%CODEC_ID_PCM)) & ")  byte[8]=ch=" & _
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

        ' ---- HARDENING (zero-day defense): validate all packet header fields ----
        ' A malformed/malicious packet could carry out-of-range values. Reject
        ' the packet (ITERATE DO) rather than risk downstream corruption.

        ' Codec: only 5 (ADPCM) and 6 (PCM) supported. Reject garbage.
        IF pktCodec <> %CODEC_ID AND pktCodec <> %CODEC_ID_PCM THEN
            IF audRecvCount <= 3 THEN
                AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                       ": REJECT pkt#" & TRIM$(STR$(audRecvCount)) & _
                       " codec=" & TRIM$(STR$(pktCodec)) & " (expected 5 or 6)"
            END IF
            ITERATE DO
        END IF

        ' Sample-rate enum: must be 0..6 (7 known EASSP rates). Reject garbage.
        ' A bad enum would silently fall back to 16000 in RateEnumToHz, causing
        ' a format-mismatch with WaveOut -> audio corruption or crash.
        IF pktRateEnum < 0 OR pktRateEnum > 6 THEN
            IF audRecvCount <= 3 THEN
                AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                       ": REJECT pkt#" & TRIM$(STR$(audRecvCount)) & _
                       " rate_enum=" & TRIM$(STR$(pktRateEnum)) & " (expected 0..6)"
            END IF
            ITERATE DO
        END IF

        ' Bits: 16 or 24 only. Clamp to 16 for any other value (graceful).
        IF pktBits <> 16 AND pktBits <> 24 THEN pktBits = 16
        ' Channels: 1 or 2 only. Clamp (graceful).
        IF pktCh < 1 THEN pktCh = 1
        IF pktCh > 2 THEN pktCh = 2

        ' ---- Startup click suppression: skip first ~1 second of audio ----
        ' On the first packet, read frame_ms (offset 9) and compute how many
        ' frames to skip (STARTUP_SKIP_MS / frame_ms). Then for each of those
        ' frames, consume the packet (keeps the socket drained, sends ACK-ish
        ' heartbeat via normal stats update) but do NOT decode or play it.
        ' This discards the mic's startup transient + stale DMA data.
        ' When the skip completes, arm a 1-second fade-in to smooth the
        ' silence→signal transition (see fadeSamplesRemaining below).
        IF skipFramesRemaining = 999 THEN
            ' First valid packet — compute skip count from frame_ms
            skipFrameMs = PEEK(BYTE, STRPTR(recvBuf) + 9)
            IF skipFrameMs < 1 THEN skipFrameMs = 20  ' guard against 0/garbage
            skipFramesRemaining = %STARTUP_SKIP_MS \ skipFrameMs
            ' Compute fade-in sample count from the actual sample rate.
            ' fadeSamplesTotal = samples in FADE_IN_MS at this rate.
            ' e.g. 32000 Hz × 1s = 32000 samples; 48000 Hz × 1s = 48000.
            fadeSamplesTotal = (CLNG(RateEnumToHz(pktRateEnum)) * %FADE_IN_MS) \ 1000
            IF fadeSamplesTotal < 1 THEN fadeSamplesTotal = 16000  ' guard
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                   ": startup skip - discarding first " & _
                   TRIM$(STR$(skipFramesRemaining)) & " frames (" & _
                   TRIM$(STR$(%STARTUP_SKIP_MS)) & "ms / " & _
                   TRIM$(STR$(skipFrameMs)) & "ms), then " & _
                   TRIM$(STR$(%FADE_IN_MS)) & "ms fade-in (" & _
                   TRIM$(STR$(fadeSamplesTotal)) & " samples) to suppress mic transient"
        END IF
        IF skipFramesRemaining > 0 THEN
            DECR skipFramesRemaining
            ' FIX (grok22#1): parse seqNum during skip and update lastSeq +
            ' pktRecv locals. Previously these stayed 0 throughout skip, so:
            '   - g_Devs(idx).wLastSeq was stale (0) during skip
            '   - After skip, the first play-packet had pktRecv=0 which
            '     DISABLED loss detection (the IF pktRecv > 0 guard), so
            '     a seq gap spanning the skip→play boundary was invisible.
            ' Now: seq is tracked during skip, lastSeq is accurate, and
            ' pktRecv is incremented so loss detection works on the FIRST
            ' play packet too. No decode/playback during skip (still skipped).
            LOCAL skipSeqNum AS WORD
            skipSeqNum = CVWRD(PEEK$(STRPTR(recvBuf), 2))
            lastSeq = skipSeqNum
            INCR pktRecv
            EnterCriticalSection g_csDev
            g_Devs(idx).dwPktRecv   = pktRecv
            g_Devs(idx).wLastSeq    = lastSeq
            g_Devs(idx).dwLastPktTick = GetTickCount()
            LeaveCriticalSection g_csDev
            ' When the LAST skipped frame is consumed, arm the fade-in so
            ' the NEXT frame (first played) starts the ramp.
            IF skipFramesRemaining = 0 THEN
                fadeSamplesRemaining = fadeSamplesTotal
                AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                       ": skip complete, fade-in armed (" & _
                       TRIM$(STR$(fadeSamplesRemaining)) & " samples / " & _
                       TRIM$(STR$(%FADE_IN_MS)) & "ms)"
            END IF
            ITERATE DO
        END IF

        ' ---- WAV dump mode ----
        ' Write decoded PCM audio to a WAV file.
        ' For PCM codec: raw payload is already PCM, write directly.
        ' For ADPCM codec: decoded PCM is in pcmPtrs after decode (handled below).
        ' First packet: determine format and open WAV file with header.
        IF g_bDumping THEN
            ' FIX (GROK-9): filter by device IP. g_dumpIp is set in
            ' ToggleDump to the currently-selected device's IP (or 0 if
            ' none selected, meaning "dump all" - legacy behavior). Without
            ' this filter, 2+ concurrent AudioThreads would all write to
            ' the single global g_hDumpFile -> byte-interleaved PCM from
            ' different devices / sample rates / channel counts -> the
            ' resulting WAV is unplayable garbage.
            IF g_dumpIp = 0 OR devIP = g_dumpIp THEN
            ' FIX (H20): protect the g_dumpWavReady check + global setup with
            ' g_csDump. The previous code only protected OpenNewDumpFile,
            ' not the check-and-set of g_dumpWavReady/g_dumpFileIdx. Two
            ' AudioThreads (two streaming devices) hitting this block
            ' simultaneously would both read g_dumpWavReady=0, both set
            ' g_dumpFileIdx=1, both call OpenNewDumpFile on the same path ->
            ' the second OPEN either fails or opens a second handle to the
            ' same file -> subsequent PUT$ calls interleave -> WAV corruption.
            EnterCriticalSection g_csDump
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
                OpenNewDumpFile
            END IF
            LeaveCriticalSection g_csDump

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
            END IF  ' FIX (GROK-9): close the g_dumpIp filter IF
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

        ' ---- Sequence number + loss detection (MOVED HERE from pcm_ready) ----
        ' Compute lostPkt/oooPkt NOW, before PLC and decode, so PLC can
        ' insert a concealment frame for the lost packet(s) BEFORE the current
        ' packet is decoded and played.
        '
        ' PREVIOUS BUG: this was computed in pcm_ready (END of iteration).
        ' lostPkt from iteration B (packet N+1, after losing N) was used in
        ' iteration C (packet N+2) — so PLC fired one packet LATE, replacing
        ' the good packet N+2 with a copy of N+1 instead of concealing the
        ' actually-lost packet N. Net effect: 2 effective losses (N dropped
        ' as silence + N+2 overwritten with a repeat) instead of 1 concealed.
        ' Now PLC fires in the SAME iteration that detects the loss, concealing
        ' N while still decoding N+1 normally → 0 effective losses on a single
        ' packet drop.
        seqNum = CVWRD(PEEK$(STRPTR(recvBuf), 2))
        lostPkt = 0
        oooPkt  = 0
        IF pktRecv > 0 THEN
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

        ' FIX (HOTRESTART-DETECT): ESP restarts seq_num from 0 on every
        ' start_streaming (cmd_hotrestart + AT+HOTRESTART, or auto-restart
        ' after watchdog). The new seq_num is small (e.g. 0..5) while our
        ' lastSeq is large (e.g. 30000). The seqDiff math above would
        ' classify this as 'out of order' (seqDiff > 32768) and discard
        ' EVERY packet from the new stream -> audio silence forever, with
        ' no error log because oooPkt is silently incremented.
        '
        ' ROBUST DETECTION (FIX: false-positive at natural seq overflow):
        ' The previous heuristic 'lastSeq > 1000 AND seqNum < 100' fired
        ' FALSE POSITIVE at natural seq_num overflow (every ~11 min at
        ' 100 pkt/s) - when seq wraps 65535->0, the heuristic reset the
        ' jitter buffer, causing an unwanted audio gap.
        '
        ' New heuristic uses BOTH seq_num AND timestamp_ms from the packet
        ' header (offset 2, uint32). ESP sets timestamp_ms = frame_count *
        ' frame_ms, RESETS TO 0 on every start_streaming. uint32_t wraps at
        ' ~49 days, so in normal operation timestamp_ms is monotonic.
        '
        ' Three conditions must ALL be true:
        '   1. pktTimestamp < 1000 ms -- packet is from a fresh stream (<1s in)
        '   2. lastSeq > 1000          -- old stream had run a while
        '   3. seqDiff > 32768         -- seq jumped BACKWARDS in modular
        '                               arithmetic (rules out natural overflow,
        '                               where seqDiff = 0 or small N for N lost)
        '
        ' Why condition 3 rules out natural overflow:
        '   - Normal wrap (65535 -> 0, no loss): seqDiff = 0
        '   - Wrap with N lost packets (65530 -> 5): seqDiff = N (small)
        '   - True HOTRESTART (30000 -> 0): seqDiff = 35534 (large, >32768)
        ' Only a true restart produces a large backward modular jump.
        LOCAL pktTimestamp AS DWORD
        pktTimestamp = PEEK(DWORD, STRPTR(recvBuf) + 2)
        IF pktRecv > 0 AND lastSeq > 1000 AND seqDiff > 32768 AND pktTimestamp < 1000 THEN
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                   ": HOTRESTART detected (seq " & TRIM$(STR$(lastSeq)) & _
                   " -> " & TRIM$(STR$(seqNum)) & ") - resetting jitter buffer"
            ' FIX (GROK-4): unified pipeline reset via macro. The previous
            ' inline block was the most complete of the 5 reset paths but
            ' still missed skipFrameMs (only set skipFramesRemaining=999).
            ' The macro sets skipFramesRemaining=999 + fadeSamplesRemaining=0
            ' + plcActive=0 + lastPcm* + pktRecv=0 + per-device stats.
            ' skipFrameMs is left at its last value (set during cold start
            ' to s_frame_ms from the ESP, typically 20ms) - it doesn't need
            ' a reset since it's only read when skipFramesRemaining > 0.
            ResetPipelineState
        END IF

        ' ---- LATE PACKET DISCARD (out-of-order) ----
        ' If packet is out-of-order (seq <= lastSeq = already played),
        ' DISCARD it. Playing a late packet produces a backwards audio segment
        ' -> click. This is what WebRTC/GStreamer do.
        IF oooPkt THEN
            EnterCriticalSection g_csDev
            g_Devs(idx).dwPktOOO    = g_Devs(idx).dwPktOOO + 1
            g_Devs(idx).dwLastPktTick = GetTickCount()
            LeaveCriticalSection g_csDev
            ITERATE DO
        END IF
        lastSeq = seqNum

        ' ---- Find a FREE buffer for the current packet's decode ----
        ' Done FIRST (before PLC) so the real packet always gets a buffer.
        ' PLC then uses a SEPARATE free buffer (skipping wIdx) — if no second
        ' free buffer is available, PLC is skipped (the lost packet becomes a
        ' natural gap rather than stealing the current packet's buffer).
        '
        ' CRITICAL FIX: the old code decoded into pcmPtrs(wIdx) WITHOUT
        ' checking if WaveOut was still playing that buffer. If whdrs(wIdx)
        ' was INQUEUE (being played), the decoder overwrote live audio ->
        ' clicks. Now we scan for a free buffer BEFORE decoding.
        '
        ' If ALL buffers are INQUEUE (underrun): we must wait. But we can't
        ' block the recv loop. So we SKIP this packet (drop it) and log the
        ' underrun. This is better than overwriting a playing buffer.
        LOCAL freeBufIdx AS LONG
        freeBufIdx = -1
        FOR wIdx2 = 0 TO %NUM_WAVE_BUFS - 1
            IF (whdrs(wIdx2).dwFlags AND %WHDR_INQUEUE) = 0 THEN
                freeBufIdx = wIdx2
                EXIT FOR
            END IF
        NEXT wIdx2
        IF freeBufIdx < 0 THEN
            ' All buffers in queue = queue FULL (overflow, not underrun).
            ' Drop this packet, count it.
            ' FIX (GROK-5): use a separate overflowCount instead of reusing
            ' underrunCount. The previous code incremented underrunCount on
            ' overflow, which (a) mislabeled the event in logs as "underrun"
            ' and (b) fed the overflow events into the adaptive-jitter grow
            ' decision at the JITTER_ADAPT_MS check, which would GROW
            ' jitterTarget when the queue was already full (counter-productive).
            ' Now overflows only increment overflowCount; the adaptive logic
            ' uses underrunCount (true underruns = empty queue) to grow, and
            ' overflowCount to SHRINK jitterTarget.
            INCR overflowCount
            IF overflowCount <= 3 OR (overflowCount MOD 100) = 0 THEN
                AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                       ": overflow (all bufs INQUEUE) - pkt dropped, overflows=" & _
                       TRIM$(STR$(overflowCount))
            END IF
            ' Re-prebuffer to rebuild the queue
            IF jitterPrebuffering = 0 THEN
                jitterPrebuffering = 1
                jitterFilled = 0
            END IF
            ITERATE DO
        END IF
        ' Use the free buffer for this frame's decode
        wIdx = freeBufIdx

        ' ---- PLC (Packet Loss Concealment) with linear interpolation ----
        ' When packets were lost (lostPkt > 0), insert ONE synthetic frame
        ' (copy of last good frame with a fade ramp) into the WaveOut queue
        ' BEFORE decoding the current packet. This conceals the lost packet N
        ' while still playing the current packet N+1 normally — the PLC frame
        ' is submitted to a SEPARATE buffer (plcBufIdx, != wIdx) so it doesn't
        ' replace the current packet.
        '
        ' PREVIOUS BUG: lostPkt was computed in pcm_ready (END of iteration),
        ' so PLC fired one packet LATE — replacing good packet N+2 with a copy
        ' of N+1 instead of concealing the lost N. Net effect was 2 effective
        ' losses (N dropped + N+2 overwritten) instead of 1 concealed. Now PLC
        ' fires in the SAME iteration that detects the loss, concealing N while
        ' decoding N+1 normally → 0 effective losses on a single packet drop.
        '
        ' Only ONE PLC frame is inserted regardless of lostPkt count, to avoid
        ' flooding the WaveOut queue and shifting playback timing. Multi-packet
        ' bursts get one concealment + the rest as natural gaps.
        plcActive = 0
        IF lostPkt > 0 AND lastPcmPtr <> 0 AND lastPcmLen > 0 AND waveOut <> 0 THEN
            ' Find a free buffer for the PLC frame, SKIPPING wIdx (reserved for
            ' the current packet). If none, skip PLC — the current packet must
            ' not be sacrificed for concealment.
            LOCAL plcBufIdx AS LONG
            plcBufIdx = -1
            FOR wIdx2 = 0 TO %NUM_WAVE_BUFS - 1
                IF wIdx2 <> wIdx AND (whdrs(wIdx2).dwFlags AND %WHDR_INQUEUE) = 0 THEN
                    plcBufIdx = wIdx2
                    EXIT FOR
                END IF
            NEXT wIdx2

            IF plcBufIdx >= 0 THEN
                ' Copy last good frame into the PLC buffer
                POKE$ pcmPtrs(plcBufIdx), PEEK$(lastPcmPtr, lastPcmLen)
                LOCAL plcLen AS LONG
                plcLen = lastPcmLen

                ' Linear interpolation: ramp from lastPcmLastSample to the
                ' first sample of the copied frame over the first N samples.
                ' Smooths the discontinuity at the splice point.
                ' FIX (BUG#R3-1): extended to stereo (nCh=2). For interleaved
                ' L,R,L,R PCM, ramp both channels at each sample index.
                ' Previously stereo fell through with NO ramp -> hard splice
                ' -> audible click on every lost packet in stereo streams.
                IF outBits = 16 AND (nCh = 1 OR nCh = 2) THEN
                    LOCAL pPcm16 AS INTEGER PTR
                    LOCAL rampLen AS LONG
                    LOCAL rampI AS LONG
                    LOCAL rampStep AS DOUBLE
                    LOCAL chStride AS LONG
                    LOCAL rampCh AS LONG
                    LOCAL chFirst AS LONG
                    LOCAL chStep AS DOUBLE
                    LOCAL chVal AS DOUBLE
                    pPcm16 = pcmPtrs(plcBufIdx)
                    chStride = nCh  ' 1 for mono, 2 for stereo (interleaved)
                    ' rampLen is in FRAMES (not bytes): 25% of frame, cap 64.
                    ' lastPcmLen is bytes; samples = bytes/2; frames = samples/chStride.
                    rampLen = (lastPcmLen \ 2) \ chStride \ 4
                    IF rampLen > 64 THEN rampLen = 64
                    IF rampLen > 0 THEN
                        ' FIX (grok22#5): ramp each channel independently.
                        ' For stereo, L ramps from lastL to firstL, R ramps
                        ' from lastR to firstR. Previously both used L's
                        ' values, causing an R discontinuity at the splice.
                        FOR rampCh = 0 TO chStride - 1
                            chFirst = @pPcm16[rampCh]  ' first sample of this ch
                            chStep = (chFirst - lastPcmLastSample(rampCh)) / rampLen
                            FOR rampI = 0 TO rampLen - 1
                                chVal = lastPcmLastSample(rampCh) + chStep * rampI
                                @pPcm16[rampI * chStride + rampCh] = CINT(chVal)
                            NEXT rampI
                        NEXT rampCh
                    END IF
                END IF

                ' Submit the PLC frame to WaveOut. It plays BEFORE the current
                ' packet (which is decoded and submitted below), concealing the
                ' lost packet's slot in the timeline.
                ' NOTE: plcActive is NOT set to 1 here — the current packet
                ' (decoded below into wIdx) is a REAL packet and SHOULD update
                ' lastPcm, so the next loss can conceal from it. The old code
                ' set plcActive=1 because PLC replaced the current buffer; now
                ' PLC uses a separate buffer, so this flag stays 0.
                whdrs(plcBufIdx).dwBufferLength = plcLen
                whdrs(plcBufIdx).dwBytesRecorded = plcLen
                waveOutWrite waveOut, whdrs(plcBufIdx), SIZEOF(WAVEHDR)
            END IF
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
                ' Bit-perfect passthrough (16->16 or 24->24): copy directly.
                pcmLen = LEN(recvBuf) - %PKT_HDR_SZ
                ' FIX (L43): log oversized PCM frames instead of silently
                ' clamping. The clamp is still applied (defensive), but the
                ' log makes a future MTU bug visible. ESP MTU limit ~1400
                ' keeps us under bufSz, but 24-bit/stereo/48k/40ms would be
                ' 11520 bytes - exceeds bufSz (7680) and would be truncated.
                IF pcmLen > bufSz THEN
                    AddLog "[AUD] PCM frame " & TRIM$(STR$(pcmLen)) & _
                           " bytes exceeds bufSz " & TRIM$(STR$(bufSz)) & _
                           " - truncating (check ESP frame_ms)"
                    pcmLen = bufSz
                END IF
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

            ' FIX (AUDIT-MEDIUM): dead diagnostic block 'first 5 packets'
            ' removed - the comment above said 'Remove this block after
            ' debugging is complete' but it had been left in for many
            ' releases, cluttering logs and wasting CPU on every stream
            ' start (5 packets x ~960 samples each = ~4800 ABS() ops).
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

        ' ---- Stats update ----
        ' NOTE: seqNum / lostPkt / oooPkt / lastSeq are now computed at the
        ' TOP of the loop (before PLC + decode), not here. Out-of-order
        ' packets are already discarded via ITERATE DO up there. By the time
        ' we reach pcm_ready, lostPkt holds the gap count for THIS packet
        ' (already used to insert the PLC frame above) and oooPkt is always 0.
        EnterCriticalSection g_csDev
        g_Devs(idx).dwPktRecv   = pktRecv
        g_Devs(idx).wLastSeq    = lastSeq
        ' FIX (M41): use CQUD to avoid sign-extension issues when adding
        ' LONG (pcmLen) to the QUAD counter. dwBytesRecv is QUAD (64-bit,
        ' see types.inc) so no 4GB wraparound - the old 'cosmetic' note is
        ' obsolete since the field type was widened from DWORD to QUAD.
        g_Devs(idx).dwBytesRecv = g_Devs(idx).dwBytesRecv + CQUD(pcmLen)
        g_Devs(idx).dwLastPktTick = GetTickCount()   ' stall detection: alive
        IF lostPkt THEN g_Devs(idx).dwPktLost = g_Devs(idx).dwPktLost + lostPkt
        LeaveCriticalSection g_csDev

        ' ---- WAV dump: ADPCM decoded PCM ----
        ' For ADPCM codec: write decoded 16-bit PCM from pcmPtrs to WAV file.
        ' (PCM codec is already handled above with raw payload write.)
        ' FIX (GROK-9): apply the same g_dumpIp filter as the PCM path above
        ' so only the selected device's decoded PCM is dumped.
        IF g_bDumping AND pktCodec = %CODEC_ID AND g_hDumpFile AND pcmLen > 0 _
           AND (g_dumpIp = 0 OR devIP = g_dumpIp) THEN
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

        ' ---- Save last decoded PCM for PLC (Packet Loss Concealment) ----
        IF pcmLen > 0 AND plcActive = 0 THEN
            lastPcmPtr = pcmPtrs(wIdx)
            lastPcmLen = pcmLen
            ' Save last sample value per channel (for interpolation on next
            ' packet loss). FIX (grok22#5): per-channel, not just L.
            ' For interleaved L,R,L,R,...,L_{n-1},R_{n-1}:
            '   last L index = (total_samples - 2)  [skip the last R]
            '   last R index = (total_samples - 1)
            ' For mono: last sample = (total_samples - 1)
            IF outBits = 16 AND pcmLen >= 2 THEN
                LOCAL pLast AS INTEGER PTR
                LOCAL totalSmp AS LONG
                pLast = pcmPtrs(wIdx)
                totalSmp = pcmLen \ 2
                IF nCh = 2 THEN
                    lastPcmLastSample(0) = @pLast[totalSmp - 2]  ' last L
                    lastPcmLastSample(1) = @pLast[totalSmp - 1]  ' last R
                ELSE
                    lastPcmLastSample(0) = @pLast[totalSmp - 1]
                END IF
            END IF
        END IF

        ' ---- Fade-in ramp (startup click suppression, 2nd second) ----
        ' Applied AFTER the 1-second skip completes. Linearly scales sample
        ' amplitude from 0 to 1 over fadeSamplesRemaining samples (1 second
        ' at the current sample rate). Operates IN-PLACE on pcmPtrs(wIdx).
        ' Handles frame boundaries: if the frame is shorter than the remaining
        ' ramp, fades the whole frame and continues on the next. If longer,
        ' fades the first N samples and leaves the rest at full amplitude.
        ' 16-bit only (ADPCM decodes to 16-bit; 24-bit PCM skips — acceptable
        ' since the skip already removed the main transient).
        IF fadeSamplesRemaining > 0 AND pcmLen > 0 AND outBits = 16 THEN
            ' FIX (BUG#R3-fade): raised-cosine fade-in instead of linear.
            ' Linear has a derivative discontinuity at the start (slope jumps
            ' from 0 to 1/N) which itself produces a faint click. Raised-cosine
            ' = 0.5 * (1 - COS(pi * progress)) has zero slope at both ends,
            ' eliminating the discontinuity. progress = (samples_faded_so_far)
            ' / fadeSamplesTotal, in [0,1]. COS cost is negligible (~16k calls
            ' for a 1s fade @ 16kHz, < 1ms total).
            LOCAL pFade AS INTEGER PTR
            LOCAL fadeFrameSamples AS LONG
            LOCAL fadeI AS LONG
            LOCAL fadeScale AS DOUBLE
            LOCAL fadeProg AS DOUBLE
            pFade = pcmPtrs(wIdx)
            fadeFrameSamples = pcmLen \ 2   ' 16-bit samples in this frame
            IF fadeFrameSamples < fadeSamplesRemaining THEN
                ' Frame shorter than remaining ramp — fade whole frame,
                ' continue ramp on next frame
                FOR fadeI = 0 TO fadeFrameSamples - 1
                    fadeProg = (fadeSamplesTotal - fadeSamplesRemaining + fadeI) / fadeSamplesTotal
                    fadeScale = 0.5# * (1.0# - COS(3.141592653589793## * fadeProg))
                    @pFade[fadeI] = CINT(@pFade[fadeI] * fadeScale)
                NEXT fadeI
                fadeSamplesRemaining = fadeSamplesRemaining - fadeFrameSamples
            ELSE
                ' Ramp completes within this frame — fade first N samples,
                ' leave the rest at full amplitude
                FOR fadeI = 0 TO fadeSamplesRemaining - 1
                    fadeProg = (fadeSamplesTotal - fadeSamplesRemaining + fadeI) / fadeSamplesTotal
                    fadeScale = 0.5# * (1.0# - COS(3.141592653589793## * fadeProg))
                    @pFade[fadeI] = CINT(@pFade[fadeI] * fadeScale)
                NEXT fadeI
                fadeSamplesRemaining = 0
                AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                       ": fade-in complete, full amplitude"
            END IF
        END IF

        ' ---- Jitter Buffer: prebuffer before starting playback ----
        IF jitterPrebuffering AND pcmLen > 0 THEN
            INCR jitterFilled
            ' During prebuffer, still submit to WaveOut (buffers queue up)
            IF waveOut <> 0 AND (whdrs(wIdx).dwFlags AND %WHDR_INQUEUE) = 0 THEN
                whdrs(wIdx).dwBufferLength = pcmLen
                whdrs(wIdx).dwBytesRecorded = pcmLen
                waveOutWrite waveOut, whdrs(wIdx), SIZEOF(WAVEHDR)
                wIdx = (wIdx + 1) MOD %NUM_WAVE_BUFS
            END IF
            IF jitterFilled >= jitterTarget THEN
                jitterPrebuffering = 0
                AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                       ": jitter buffer filled (" & TRIM$(STR$(jitterFilled)) & _
                       " frames) - playback started"
            END IF
            ITERATE DO
        END IF

        ' ---- Adaptive buffer adjustment ----
        ' Every JITTER_ADAPT_MS, adjust jitterTarget based on underruns.
        ' underrunCount is now correctly incremented when WaveOut queue is
        ' empty (0 INQUEUE). If underruns happen, increase prebuffer; if
        ' stable, slowly decrease to reduce latency.
        ' FIX (BUG#R3-2): do NOT decrease jitterTarget in the first 5 seconds
        ' after stream start. Early WiFi jitter is normal (DNS, DHCP renewal,
        ' ESP I2S DMA warmup) and a too-small prebuffer during this window
        ' causes underrun-clicks. The decrease guard is one-shot per stream.
        IF (GetTickCount() - lastAdaptTick) >= %JITTER_ADAPT_MS THEN
            LOCAL msSinceStart AS DWORD
            msSinceStart = GetTickCount() - g_Devs(idx).dwStreamStart
            ' FIX (log-fix-C): require 2 consecutive zero-underrun intervals
            ' (20s) before decreasing. Log showed the buffer oscillating
            ' 6->5->7->6->5->4 because a single 10s zero-underrun window
            ' was followed by underruns. Requiring 2 windows confirms the
            ' buffer is genuinely too large, not just in a lucky gap.
            IF underrunCount = 0 AND jitterTarget > %JITTER_MIN AND _
               msSinceStart > 5000 THEN
                INCR zeroUnderrunIntervals
                IF zeroUnderrunIntervals >= 2 THEN
                    DECR jitterTarget
                    zeroUnderrunIntervals = 0
                    AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                           ": adaptive buffer: 0 underruns -> decrease to " & _
                           TRIM$(STR$(jitterTarget))
                END IF
            ELSEIF underrunCount >= %JITTER_UNDERRUN_THRESHOLD AND _
                   jitterTarget < %JITTER_MAX THEN
                INCR jitterTarget
                zeroUnderrunIntervals = 0
                AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                       ": adaptive buffer: " & TRIM$(STR$(underrunCount)) & _
                       " underruns -> increase to " & TRIM$(STR$(jitterTarget))
            ELSE
                ' Underruns below threshold or at JITTER_MAX: reset counter
                zeroUnderrunIntervals = 0
            END IF
            underrunCount = 0
            ' FIX (GROK-5): also reset overflowCount at end of adaptation interval
            overflowCount = 0
            lastAdaptTick = GetTickCount()
        END IF

        ' ---- Underrun detection (CORRECTED: 0 INQUEUE = underrun, not all) ----
        ' OLD BUG: checked if ALL buffers were INQUEUE (= overflow). But the
        ' real underrun = ZERO INQUEUE (WaveOut queue empty, playing silence).
        ' This happened when network jitter > prebuffer depth: WaveOut drained
        ' all queued buffers, played silence for a few ms, then the late packet
        ' arrived → discontinuity → CLICK. With 0 drops and 0 lost packets
        ' (the packet wasn't lost, just late), this was invisible to the stats.
        '
        ' FIX: count INQUEUE buffers. If 0 INQUEUE (not prebuffering) → underrun.
        ' On underrun: re-prebuffer + arm a short fade-in (5ms) to smooth the
        ' silence→signal transition when playback resumes.
        IF waveOut <> 0 AND pcmLen > 0 AND jitterPrebuffering = 0 THEN
            LOCAL inQueueCount AS LONG
            inQueueCount = 0
            FOR wIdx2 = 0 TO %NUM_WAVE_BUFS - 1
                IF (whdrs(wIdx2).dwFlags AND %WHDR_INQUEUE) <> 0 THEN
                    INCR inQueueCount
                END IF
            NEXT wIdx2
            IF inQueueCount = 0 THEN
                ' UNDERRUN: WaveOut queue is empty, was playing silence.
                ' Re-prebuffer to rebuild the queue, and arm a short fade-in
                ' to prevent the click when audio resumes.
                INCR underrunCount
                jitterPrebuffering = 1
                jitterFilled = 0
                ' Arm a short fade-in (5ms = 160 samples at 32kHz) for the
                ' resume. This smooths the silence->signal discontinuity.
                IF fadeSamplesRemaining = 0 AND outBits = 16 THEN
                    fadeSamplesTotal = 160
                    fadeSamplesRemaining = 160
                END IF
                IF underrunCount <= 3 OR (underrunCount MOD 50) = 0 THEN
                    AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                           ": underrun (0 bufs in queue) - re-prebuffering #" & _
                           TRIM$(STR$(underrunCount))
                END IF
                ' FIX (GROK-6): ITERATE DO here so the current packet is
                ' DROPPED into the prebuffer phase (next iteration handles
                ' it via the jitterPrebuffering branch). Previously the code
                ' fell through to waveOutWrite, submitting the current
                ' packet raw into the empty queue -> click + immediate
                ' repeat underrun risk. With ITERATE DO, the prebuffer
                ' phase rebuilds queue depth before the next submit.
                ITERATE DO
            END IF
        END IF

        ' ---- Check for WaveOut reopen request (device change / sleep-wake) ----
        ' Set by WM_DEVICECHANGE / WM_POWERBROADCAST handlers in MainDlgProc.
        ' When the HDMI audio device is re-initialized after sleep/wake, the
        ' old waveOut handle becomes invalid and waveOutWrite silently fails.
        ' We reopen WaveOut (close, find device by ID, open, prepare headers)
        ' by jumping to the existing reopen_waveout label. The format stays
        ' the same — the reopen path uses the current devCodec/nCh/devBits.
        '
        ' CRITICAL: On sleep/wake (device change / resume), the TCP connection
        ' is ALSO dead — Windows doesn't send FIN during sleep, so the server's
        ' TCP RECV blocks forever waiting for data that will never arrive. The
        ' ESP keeps sending (its send() doesn't fail because SO_SNDTIMEO only
        ' triggers when the send buffer is full), but the server never receives.
        ' Symptom: "42000 sent, 0 dropped" on ESP, but no audio on server.
        '
        ' Fix: when reopening WaveOut due to device change / resume, also close
        ' and reconnect the TCP socket. This breaks the dead connection and
        ' establishes a fresh one that actually receives data.
        ' FIX (BUG#4+#5): atomic read-modify-write of bReopenWaveOut AND
        ' dwLastReopenTick under a single CS hold. Previously bReopenWaveOut
        ' was read OUTSIDE CS (line 2702), then CS entered only to clear it.
        ' A set by GUI between the outside-read and the inside-clear was
        ' LOST -> WaveOut not reopened after sleep/wake -> silent audio.
        ' dwLastReopenTick was also read/written outside CS, so the 5s
        ' suppress window was inconsistent between GUI timers and AudioThread.
        ' Now: enter CS, read+clear flag, check suppress, write tick, leave
        ' CS - all atomic. The actual reopen (TCP CLOSE, waveOutClose) runs
        ' outside CS (no shared state touched, and we never hold CS during
        ' blocking I/O).
        LOCAL bDoReopen AS LONG
        bDoReopen = 0
        LOCAL nowReopenTick AS DWORD
        nowReopenTick = GetTickCount()
        EnterCriticalSection g_csDev
        IF g_Devs(idx).bReopenWaveOut THEN
            g_Devs(idx).bReopenWaveOut = 0
            ' Suppress duplicate reopen within 5 seconds — after sleep/wake,
            ' multiple sources (waveOutWrite error, WM_DEVICECHANGE debounce,
            ' WM_POWERBROADCAST) all set bReopenWaveOut=1, causing 2-3
            ' consecutive reopens. Only the first should proceed; the rest
            ' are redundant (WaveOut was just reopened, it's fine).
            IF g_Devs(idx).dwLastReopenTick > 0 AND _
               (nowReopenTick - g_Devs(idx).dwLastReopenTick) < 5000 THEN
                LeaveCriticalSection g_csDev
                AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                       ": reopen suppressed (duplicate within 5s)"
                ' Skip reopen — WaveOut was just reopened, it's still valid.
                ' But still reset jitter state (packets may have been lost).
                jitterPrebuffering = 1
                jitterFilled = 0
                ITERATE DO
            END IF
            g_Devs(idx).dwLastReopenTick = nowReopenTick
            bDoReopen = 1
        END IF
        LeaveCriticalSection g_csDev
        IF bDoReopen = 0 THEN GOTO skip_reopen_block
        AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
               ": reopening WaveOut + TCP (device change / resume)"
            ' Close the dead TCP socket (if TCP transport)
            IF devTransport = 1 AND fNum > 0 THEN
                TCP CLOSE #fNum
                ERRCLEAR
                AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                       ": TCP closed for reconnect (sleep/wake recovery)"
                ' Reconnect TCP before WaveOut reopen — the reopen_waveout path
                ' will skip the TCP connect (it only handles WaveOut). We need
                ' a fresh TCP connection first.
                LOCAL reconOk2 AS LONG
                reconOk2 = 0
                DO WHILE g_Devs(idx).dwRunning
                    ERRCLEAR
                    TCP OPEN PORT bindPort AT tcpHost AS #fNum TIMEOUT 2000
                    IF ERR = 0 THEN reconOk2 = 1 : EXIT DO
                    SLEEP 1000
                LOOP
                IF reconOk2 = 0 THEN EXIT DO  ' stream stopped
                AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                       ": TCP reconnected for sleep/wake recovery"
                ' FIX (GROK-4): unified pipeline reset via macro. Previously
                ' this path reset only predictor/jitter/lastSeq but NOT
                ' skip/fade/lastPcm/pktRecv -> false OOO on seqNum > 32768
                ' -> silence forever after sleep/wake. The macro now resets
                ' ALL state, so the post-reopen playback re-prebuffers cleanly.
                ResetPipelineState
                ' Reopen WaveOut inline (NOT goto reopen_waveout - that path
                ' does a full startup skip + state reset which would discard
                ' all incoming packets for 1 second. We just need to reopen
                ' the WaveOut device, not reset the entire audio pipeline).
                IF waveOut <> 0 THEN
                    LOCAL devReopenI AS LONG
                    waveOutReset waveOut
                    FOR devReopenI = 0 TO %NUM_WAVE_BUFS - 1
                        waveOutUnprepareHeader waveOut, whdrs(devReopenI), SIZEOF(WAVEHDR)
                    NEXT devReopenI
                    waveOutClose waveOut
                    waveOut = 0
                    EnterCriticalSection g_csDev
                    g_Devs(idx).hWaveOut = 0
                    LeaveCriticalSection g_csDev
                END IF
                ' Re-open WaveOut with same format
                ' FIX (GROK-8): re-snapshot devWaveDevice under CS in case
                ' the user changed output device since the stream started.
                EnterCriticalSection g_csDev
                devWaveDevice = g_Devs(idx).dwWaveDevice
                LeaveCriticalSection g_csDev
                waveResult = waveOutOpen(waveOut, devWaveDevice, wfFmt, 0, 0, %CALLBACK_NULL)
                IF waveResult <> 0 AND outBits = 24 THEN
                    outBits = 16
                    wfFmt.wBitsPerSample  = 16
                    wfFmt.nBlockAlign     = nCh * 2
                    wfFmt.nAvgBytesPerSec = wfFmt.nSamplesPerSec * wfFmt.nBlockAlign
                    waveResult = waveOutOpen(waveOut, devWaveDevice, wfFmt, 0, 0, %CALLBACK_NULL)
                END IF
                IF waveResult = 0 THEN
                    EnterCriticalSection g_csDev
                    g_Devs(idx).hWaveOut = waveOut
                    LeaveCriticalSection g_csDev
                    FOR devReopenI = 0 TO %NUM_WAVE_BUFS - 1
                        whdrs(devReopenI).dwFlags = 0
                        whdrs(devReopenI).dwBufferLength = bufSz
                        ' FIX (BUG#15): check prepare result on reopen
                        prepRes = waveOutPrepareHeader(waveOut, whdrs(devReopenI), SIZEOF(WAVEHDR))
                        IF prepRes <> 0 THEN
                            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                                   ": waveOutPrepareHeader FAILED (reopen) buf=" & _
                                   TRIM$(STR$(devReopenI)) & " err=" & TRIM$(STR$(prepRes))
                            whdrs(devReopenI).dwFlags = whdrs(devReopenI).dwFlags OR %WHDR_INQUEUE
                        END IF
                    NEXT devReopenI
                    AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                           ": WaveOut reopened " & TRIM$(STR$(outBits)) & "-bit " & _
                           CodecName(devCodec) & " " & TRIM$(STR$(devSmpRate)) & "Hz " & _
                           TRIM$(STR$(nCh)) & "ch"
                ELSE
                    AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                           ": WaveOut reopen FAILED err=" & TRIM$(STR$(waveResult))
                END IF
                ' Continue main loop — new TCP data will arrive via TCP RECV
                ITERATE DO
            END IF
            ' For non-TCP (UDP) transport: use the old reopen_waveout path
            GOTO reopen_waveout
        ' FIX (BUG#4+#5): the original END IF here closed the outer
        ' IF g_Devs(idx).bReopenWaveOut THEN which was replaced by the
        ' atomic-RMW + GOTO skip_reopen_block pattern above. Removed.
        ' Label target for the early-out when no reopen was requested.
        ' Falls through to the WaveOut submit block.
skip_reopen_block:

        ' ---- Submit to WaveOut ----
        ' wIdx already points to a FREE buffer (found in the scan above).
        ' Submit the decoded PCM for playback.
        IF waveOut <> 0 AND pcmLen > 0 THEN
            whdrs(wIdx).dwBufferLength = pcmLen
            whdrs(wIdx).dwBytesRecorded = pcmLen
            LOCAL wWriteRes AS LONG
            wWriteRes = waveOutWrite(waveOut, whdrs(wIdx), SIZEOF(WAVEHDR))
            ' Check for device-loss errors. After sleep/wake the waveOut handle
            ' becomes invalid; waveOutWrite returns MMSYSERR_NODRIVER (2) or
            ' MMSYSERR_INVALHANDLE (9). Flag reopen instead of silently losing
            ' audio. MMSYSERR_NOERROR = 0, WAVERR_STILLPLAYING = 33 (buffer still
            ' benign — don't reopen on that).
            IF wWriteRes <> 0 AND wWriteRes <> 33 THEN
                AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                       ": waveOutWrite failed err=" & TRIM$(STR$(wWriteRes)) & _
                       " - flagging reopen"
                ' FIX (H15): protect bReopenWaveOut write with CS

                EnterCriticalSection g_csDev

                g_Devs(idx).bReopenWaveOut = 1

                LeaveCriticalSection g_csDev
            END IF
        END IF

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

        ' Reset jitter buffer state for new format
        jitterPrebuffering = 1
        jitterFilled = 0
        underrunCount = 0
        overflowCount = 0   ' FIX (GROK-5)
        zeroUnderrunIntervals = 0   ' FIX (log-fix-C)
        lastAdaptTick = GetTickCount()
        lastPcmPtr = 0
        lastPcmLen = 0
        plcActive = 0
        ' Re-arm startup skip (format change = new stream = mic may re-transient)
        skipFramesRemaining = 999
        ' Reset fade-in (will be armed when skip completes)

        ' ---- Close the old (stale) WaveOut if still open ----
        ' On format-change path the caller already closed it; on device-change
        ' reopen (WM_DEVICECHANGE / waveOutWrite error) we need to close it
        ' here. Safe no-op if waveOut = 0.
        IF waveOut <> 0 THEN
            LOCAL reopenI AS LONG
            waveOutReset waveOut
            FOR reopenI = 0 TO %NUM_WAVE_BUFS - 1
                waveOutUnprepareHeader waveOut, whdrs(reopenI), SIZEOF(WAVEHDR)
            NEXT reopenI
            waveOutClose waveOut
            waveOut = 0
            EnterCriticalSection g_csDev
            g_Devs(idx).hWaveOut = 0
            LeaveCriticalSection g_csDev
        END IF
        fadeSamplesRemaining = 0

        ' Set up WAVEFORMATEX - bits per sample depends on CODEC:
        '   ADPCM (codec=5): ALWAYS 16-bit (ESP dithers 24->16 before encoding)
        '   PCM (codec=6): uses devBits (16 or 24) from packet
        IF devCodec = %CODEC_ID_PCM THEN
            outBits = devBits
        ELSE
            outBits = 16
        END IF

        ' <<<DRIFT FIX>>> та же коррекция частоты при reopen
        actualSmpRate = EspActualRate(devSmpRate, devBits)
        IF actualSmpRate <> devSmpRate THEN
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                   ": rate trim " & TRIM$(STR$(devSmpRate)) & "Hz -> " & _
                   TRIM$(STR$(actualSmpRate)) & "Hz (reopen, drift fix)"
        END IF
        wfFmt.wFormatTag      = %WAVE_FORMAT_PCM
        wfFmt.nChannels       = nCh
        wfFmt.nSamplesPerSec  = actualSmpRate
        wfFmt.wBitsPerSample  = outBits
        wfFmt.nBlockAlign     = nCh * (outBits \ 8)
        wfFmt.nAvgBytesPerSec = wfFmt.nSamplesPerSec * wfFmt.nBlockAlign
        wfFmt.cbSize          = 0

        ' Open WaveOut. Try 24-bit first if PCM 24-bit; if the card rejects
        ' it, retry with 16-bit and mark for on-the-fly 24->16 conversion.
        ' FIX (GROK-8): re-snapshot devWaveDevice under CS (format change
        ' reopen path - user may have changed output device mid-stream).
        EnterCriticalSection g_csDev
        devWaveDevice = g_Devs(idx).dwWaveDevice
        LeaveCriticalSection g_csDev
        waveResult = waveOutOpen(waveOut, devWaveDevice, wfFmt, 0, 0, %CALLBACK_NULL)
        IF waveResult <> 0 AND outBits = 24 THEN
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                   ": 24-bit WaveOut failed on reopen (err=" & TRIM$(STR$(waveResult)) & _
                   "), falling back to 16-bit"
            outBits = 16
            wfFmt.wBitsPerSample  = 16
            wfFmt.nBlockAlign     = nCh * 2
            wfFmt.nAvgBytesPerSec = wfFmt.nSamplesPerSec * wfFmt.nBlockAlign
            waveResult = waveOutOpen(waveOut, devWaveDevice, wfFmt, 0, 0, %CALLBACK_NULL)
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
                ' FIX (BUG#15): check prepare result on format-change reopen
                prepRes = waveOutPrepareHeader(waveOut, whdrs(wIdx2), SIZEOF(WAVEHDR))
                IF prepRes <> 0 THEN
                    AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                           ": waveOutPrepareHeader FAILED (fmt-change) buf=" & _
                           TRIM$(STR$(wIdx2)) & " err=" & TRIM$(STR$(prepRes))
                    whdrs(wIdx2).dwFlags = whdrs(wIdx2).dwFlags OR %WHDR_INQUEUE
                END IF
            NEXT wIdx2
        ELSE
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                   ": waveOutOpen FAILED on reopen err=" & TRIM$(STR$(waveResult)) & _
                   " - audio disabled until next format change"
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

    ' Transport may have been closed from outside (StopAllStreams) on shutdown,
    ' so use ERRCLEAR to handle already-closed file gracefully.
    ' TCP and UDP use different CLOSE statements.
    ERRCLEAR
    IF devTransport = 1 THEN
        TCP CLOSE #fNum
    ELSE
        UDP CLOSE #fNum
    END IF
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

' ===========================================================================
' StartStream - Start audio stream for ONE device (no ListView interaction).
' FIX (BUG#1): extracted from StartCheckedStreams so callers can restart a
' single device without disturbing others. Previously auto-restart on
' transport change called StopCheckedStreams + StartCheckedStreams, which
' stopped and restarted EVERY checked device - dropping audio on all of them
' just because one switched UDP<->TCP. Now UpdateDevice can call
' StopStream(idx) + StartStream(idx) to affect only the changed device.
' Returns 1 on success, 0 on skip/failure.
' ===========================================================================
FUNCTION StartStream(BYVAL devIdx AS LONG) AS LONG
    LOCAL saveIP AS DWORD
    LOCAL savePort AS DWORD
    LOCAL saveMac AS STRING
    LOCAL saveAudioPort AS DWORD
    LOCAL saveSmpRate AS DWORD
    LOCAL hThread AS DWORD
    LOCAL cfgRetry AS LONG

    IF devIdx < 0 OR devIdx >= %MAX_DEVICES THEN FUNCTION = 0 : EXIT FUNCTION

    EnterCriticalSection g_csDev

    ' Skip if not active or already streaming
    ' FIX (GROK-3): also check hAudioThread<>0 to catch the case where
    ' StopAllStreams/StopStream signalled stop but the thread is still
    ' alive in cleanup (we wait up to 30s, but a timeout could leave an
    ' orphan). Refusing to start until the handle is cleared prevents
    ' duplicate AudioThreads on the same idx/port.
    IF g_Devs(devIdx).dwActive = 0 OR g_Devs(devIdx).dwHBActive <> 0 _
       OR g_Devs(devIdx).hAudioThread <> 0 THEN
        LeaveCriticalSection g_csDev
        FUNCTION = 0
        EXIT FUNCTION
    END IF

    g_Devs(devIdx).dwStatus    = %STS_STREAM
    g_Devs(devIdx).dwError     = 0
    g_Devs(devIdx).dwAudioPort = %AUDIO_PORT_BASE + devIdx

    ' Save values needed outside CS
    saveIP        = g_Devs(devIdx).dwIP
    savePort      = g_Devs(devIdx).dwPort
    saveMac       = TRIM$(g_Devs(devIdx).sMac)
    saveAudioPort = g_Devs(devIdx).dwAudioPort
    ' FIX (BUG#7): snapshot dwSmpRate here while holding CS; the old
    ' StartCheckedStreams read it OUTSIDE CS at log time, racing with
    ' UpdateDevice which can change it on the next INFO packet.
    saveSmpRate   = g_Devs(devIdx).dwSmpRate

    ' FIX (GROK-1): set dwRunning=1 HERE (under CS, BEFORE THREAD CREATE).
    ' Previously dwRunning was only set inside AudioThread after WaveOut
    ' opened. The TCP connect loop's abort condition
    '   IF g_Devs(idx).dwRunning = 0 AND connectAttempts > 1 THEN EXIT FUNCTION
    ' then fired on the 2nd failed attempt because dwRunning was still 0
    ' from cold start - breaking the "retry until success" promise and
    ' giving up after ~2 attempts. With dwRunning=1 set here, the abort
    ' only fires when StopStream/StopAllStreams explicitly clears it.
    g_Devs(devIdx).dwRunning   = 1

    ' FIX (Bug #1): seed dwStreamStart HERE so the first HeartbeatThread
    ' iteration doesn't compute elapsed = tick - 0 = "51 656 639 ms" and
    ' wrongly log "no audio after start (initial CONFIGURE lost?)". The
    ' AudioThread later overwrites dwStreamStart when it has opened its
    ' transport + WaveOut - that's the real "audio-ready" timestamp -
    ' but we need a sane value NOW in case HB fires before AudioThread
    ' reaches that line.
    g_Devs(devIdx).dwStreamStart = GetTickCount()
    g_Devs(devIdx).dwLastPktTick = g_Devs(devIdx).dwStreamStart

    LeaveCriticalSection g_csDev

    ' FIX (GROK-2): send CONFIGURE BEFORE THREAD CREATE so the ESP has
    ' already started opening its TCP listen socket by the time AudioThread
    ' issues TCP OPEN. The previous order (THREAD CREATE -> dwHBActive=1
    ' -> SLEEP 50 -> SendConfigure x3) let AudioThread begin TCP connect
    ' up to ~650ms BEFORE the first CONFIGURE reached the ESP, so the ESP
    ' hadn't opened its listen socket yet -> first connect always failed.
    ' Combined with the GROK-1 retry bug, this made TCP cold-start very
    ' fragile. Now: send CONFIGURE first (3x with 200ms gaps), then a
    ' short SLEEP 50 to let the ESP's tcp_stream_init_listen run, then
    ' THREAD CREATE.
    AddLog "[HB] sending CONFIGURE for dev #" & TRIM$(STR$(devIdx)) & _
           " - pre-thread (TCP listen setup)"
    FOR cfgRetry = 1 TO 3
        SendConfigure saveIP, savePort, saveAudioPort
        IF cfgRetry < 3 THEN SLEEP 200
    NEXT cfgRetry

    SLEEP 50

    ' Create thread outside CS
    THREAD CREATE AudioThread(devIdx) TO hThread

    ' FIX (H17): check thread creation success. Without this, if THREAD
    ' CREATE fails (hThread=0) we still set dwHBActive=1 -> UI shows
    ' 'Streaming' but no audio thread exists -> user stranded with no
    ' error. Recover by clearing state and reporting failure.
    IF hThread = 0 THEN
        AddLog "[HB] FATAL: THREAD CREATE AudioThread failed for dev #" & TRIM$(STR$(devIdx))
        EnterCriticalSection g_csDev
        g_Devs(devIdx).dwHBActive   = 0
        g_Devs(devIdx).dwStatus     = %STS_IDLE
        ' FIX (grok22#3): do NOT clear dwActive. dwActive means
        ' "device is discovered and visible in the list". A THREAD
        ' CREATE failure (low memory, handle exhaustion) is a stream-
        ' level error, not a discovery-level error. Clearing dwActive
        ' would make the device disappear from the UI even though it's
        ' still sending INFO packets and could be started again later.
        ' Only reset stream state (dwHBActive, dwStatus, hAudioThread).
        g_Devs(devIdx).hAudioThread = 0
        LeaveCriticalSection g_csDev
        FUNCTION = 0
        EXIT FUNCTION
    END IF

    ' Store thread handle + mark streaming under CS
    ' (dwHBActive set AFTER thread creation so stop logic can find hAudioThread)
    EnterCriticalSection g_csDev
    g_Devs(devIdx).hAudioThread = hThread
    g_Devs(devIdx).dwHBActive   = 1
    LeaveCriticalSection g_csDev

    AddLog "[HB] dwHBActive=1 for dev #" & TRIM$(STR$(devIdx)) & _
           " - heartbeat will be sent every " & TRIM$(STR$(%HB_INTERVAL)) & "ms"

    AddLog "Stream started: " & saveMac & " -> " & _
           FormatIP(saveIP) & ":" & TRIM$(STR$(savePort)) & _
           " port=" & TRIM$(STR$(saveAudioPort)) & _
           " rate=" & TRIM$(STR$(saveSmpRate)) & " Hz"
    FUNCTION = 1
END FUNCTION

' ===========================================================================
' StopStream - Stop audio stream for ONE device (no ListView interaction).
' FIX (BUG#1): extracted from StopCheckedStreams so callers can stop a
'               single device. Returns 1 if a stream was stopped, 0 otherwise.
' ===========================================================================
FUNCTION StopStream(BYVAL devIdx AS LONG) AS LONG
    LOCAL fNum AS LONG
    LOCAL hThread AS DWORD
    LOCAL lRes AS LONG
    LOCAL sMac AS STRING
    LOCAL stopIP AS DWORD
    LOCAL stopPort AS DWORD

    IF devIdx < 0 OR devIdx >= %MAX_DEVICES THEN FUNCTION = 0 : EXIT FUNCTION

    ' FIX (AUDIT-W2-FIX): if a previous StopStream timed out and left an
    ' orphan thread handle in g_Devs (dwHBActive=0 but hAudioThread<>0),
    ' the orphan has had time to self-exit by now (dwRunning was cleared
    ' in that prior call). Reap it: wait briefly, close the handle, clear
    ' the slot. This unblocks StartStream (whose guard is hAudioThread<>0)
    ' so the device can be restarted WITHOUT restarting the whole program.
    ' Returns 1 so a concurrent StartStream right after this will succeed.
    EnterCriticalSection g_csDev
    IF g_Devs(devIdx).dwHBActive = 0 THEN
        IF g_Devs(devIdx).hAudioThread <> 0 THEN
            hThread = g_Devs(devIdx).hAudioThread
            g_Devs(devIdx).hAudioThread = 0
            LeaveCriticalSection g_csDev
            WaitForSingleObject hThread, 5000
            THREAD CLOSE hThread TO lRes
            AddLog "[AUD] dev #" & TRIM$(STR$(devIdx)) & _
                   ": StopStream: reaped orphan AudioThread handle (now restartable)"
            FUNCTION = 1
            EXIT FUNCTION
        END IF
        LeaveCriticalSection g_csDev
        FUNCTION = 0
        EXIT FUNCTION
    END IF
    ' Capture IP/Port BEFORE clearing dwHBActive for SendStop
    stopIP   = g_Devs(devIdx).dwIP
    stopPort = g_Devs(devIdx).dwPort
    g_Devs(devIdx).dwRunning  = 0
    g_Devs(devIdx).dwHBActive = 0
    g_Devs(devIdx).dwStatus   = %STS_IDLE
    fNum = g_Devs(devIdx).fAudioFile
    g_Devs(devIdx).fAudioFile = 0
    sMac = TRIM$(g_Devs(devIdx).sMac)
    hThread = g_Devs(devIdx).hAudioThread
    g_Devs(devIdx).hAudioThread = 0
    LeaveCriticalSection g_csDev

    AddLog "[HB] dwHBActive=0 for dev #" & TRIM$(STR$(devIdx)) & _
           " - heartbeat STOPPED (single stop)"

    ' Send explicit CMD_STOP to device so it stops immediately
    ' (instead of waiting for watchdog timeout)
    SendStop stopIP, stopPort

    ' FIX (UDP stop crash): Do NOT close the audio socket here. The
    ' AudioThread OWNS this socket and may be blocked inside UDP RECV on
    ' it right now. Calling UDP CLOSE #fNum from this (GUI) thread while
    ' RECV is in progress races on PowerBASIC's process-global file-number
    ' table (CLOSE frees the slot while RECV still references it ->
    ' use-after-free -> instant crash). The AudioThread polls dwRunning
    ' every <=500ms (UDP RECV timeout), sees 0, and closes its OWN socket
    ' in its cleanup section. We just wait for the thread below.
    ' (fNum is captured only for diagnostics / bookkeeping; fAudioFile is
    '  cleared above so a restart won't see a stale handle.)

    AddLog "Stream stopping: " & sMac

    ' Wait for the audio thread to finish (it closes its own socket).
    ' FIX (grok22#2): previously used a single WaitForSingleObject with
    ' 3s timeout. If the thread was stuck in TCP reconnect (up to 2s per
    ' attempt × many retries), the wait timed out, THREAD CLOSE closed the
    ' handle but did NOT kill the thread. A subsequent StartStream would
    ' create a SECOND AudioThread for the same device → both trying to
    ' open the same port → duplicate handles / audio corruption / crash.
    '
    ' Now: poll WaitForSingleObject(hThread, 1000) in a loop up to 30s
    ' (same pattern as WaitForAllThreads). If still alive after 30s, log
    ' a warning and return 0 (failure) so the caller does NOT restart.
    ' The orphan thread will eventually exit on its own (dwRunning=0), but
    ' we don't risk a duplicate by starting a new one.
    IF hThread THEN
        LOCAL stopWaitMs AS LONG
        LOCAL stopWaitRes AS LONG
        LOCAL stopThreadAlive AS LONG
        stopWaitMs = 0
        stopThreadAlive = 0
        DO
            stopWaitRes = WaitForSingleObject(hThread, 1000)
            ' FIX (AUDIT-WAITFAILED): also exit on WAIT_FAILED (invalid
            ' handle) - otherwise the loop burns 30s at 100% CPU because
            ' WAIT_FAILED returns instantly but the old code only checked
            ' WAIT_OBJECT_0.
            IF stopWaitRes = %WAIT_OBJECT_0 OR stopWaitRes = %WAIT_FAILED THEN EXIT DO
            stopWaitMs = stopWaitMs + 1000
            IF stopWaitMs >= 30000 THEN
                stopThreadAlive = 1
                EXIT DO
            END IF
        LOOP
        IF stopThreadAlive THEN
            ' FIX (AUDIT-W2): restore hAudioThread (cleared above) so
            ' StartStream's guard (hAudioThread <> 0) refuses to create a
            ' duplicate AudioThread on the same port. Do NOT THREAD CLOSE
            ' the handle here - the orphan thread self-exits when it next
            ' polls dwRunning=0 (already set).
            ' FIX (AUDIT-W2-FIX): the reaped handle is closed on the NEXT
            ' StopStream call (orphan-reaping block at the top of this
            ' function) or by WaitForAllThreads at program exit. This
            ' avoids the leak where a restored handle was never closed and
            ' the device stayed blocked until program restart.
            EnterCriticalSection g_csDev
            g_Devs(devIdx).hAudioThread = hThread
            LeaveCriticalSection g_csDev
            AddLog "[AUD] dev #" & TRIM$(STR$(devIdx)) & _
                   ": StopStream: AudioThread did not exit in 30s - " & _
                   "NOT restarting (orphan thread will self-exit). " & _
                   "Handle preserved so StartStream blocks until exit."
            FUNCTION = 0   ' failure - caller should not restart
            EXIT FUNCTION
        END IF
        THREAD CLOSE hThread TO lRes
    END IF

    FUNCTION = 1
END FUNCTION

' StartCheckedStreams - Start audio stream for all checked devices.
' FIX (BUG#1): now a thin wrapper around StartStream(idx). The previous
' monolithic version mixed per-device logic with ListView iteration,
' making it impossible to restart just one device on transport change.
SUB StartCheckedStreams()
    LOCAL hLV AS DWORD
    LOCAL i AS LONG
    LOCAL nCount AS LONG
    LOCAL devIdx AS LONG
    LOCAL lvi AS LV_ITM
    LOCAL started AS LONG

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

        IF StartStream(devIdx) THEN INCR started
    NEXT i

    IF started = 0 THEN
        AddLog "No devices to start (not checked or already streaming)"
    ELSE
        AddLog "Started" & STR$(started) & " stream(s)"
    END IF
END SUB

' StopCheckedStreams - Stop audio stream for all checked streaming devices.
' FIX (BUG#1): now a thin wrapper around StopStream(idx). The previous
' two-phase monolith (signal-all-then-wait-all) is preserved by calling
' StopStream per device, which signals AND waits before returning. This
' serializes stop operations (slower for many devices) but is far simpler
' and avoids the duplicated ListView-iteration code. The two-phase approach
' was originally needed so all sockets got CMD_STOP quickly; in practice
' the 50ms between stops is negligible vs the 3s thread-wait timeout.
SUB StopCheckedStreams()
    LOCAL hLV AS DWORD
    LOCAL i AS LONG
    LOCAL nCount AS LONG
    LOCAL devIdx AS LONG
    LOCAL lvi AS LV_ITM
    LOCAL stopped AS LONG

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

        IF StopStream(devIdx) THEN INCR stopped
    NEXT i
END SUB

' StopAllStreams - Stop all active streams, unblock audio UDP recv
SUB StopAllStreams()
    LOCAL idx AS LONG
    DIM fNums(%MAX_DEVICES - 1) AS LONG
    DIM stopIPs(%MAX_DEVICES - 1) AS DWORD
    DIM stopPorts(%MAX_DEVICES - 1) AS DWORD
    ' FIX (H16): collect log lines to emit AFTER leaving CS. The codebase's
    ' own invariant says 'AddLog OUTSIDE CS - never hold CS during UI
    ' operations'. AddLog -> HeapAlloc + PostMessage; holding CS during
    ' HeapAlloc is unsafe, and if AddLog is ever changed to SendMessage
    ' this becomes an instant deadlock.
    DIM logLines(%MAX_DEVICES - 1) AS STRING
    DIM logCount AS LONG
    ' FIX (GROK-3): capture thread handles so we can wait on them AFTER
    ' leaving CS. The old code only signalled stop (dwRunning=0) and
    ' returned immediately, leaving AudioThreads alive in UDP RECV / TCP
    ' reconnect. The UI then enabled Start (because dwHBActive=0), and
    ' StartStream's precondition only checked dwHBActive - so the user
    ' could click Start and a SECOND AudioThread would be created on the
    ' same idx/port while the old one was still alive -> duplicate socket
    ' / port conflict / crash. Now we wait per-device (same pattern as
    ' StopStream) so StopAllStreams doesn't return until all threads are
    ' actually dead (or 30s timeout).
    DIM waitThreads(%MAX_DEVICES - 1) AS DWORD
    DIM waitDevs(%MAX_DEVICES - 1) AS LONG
    DIM waitCount AS LONG
    waitCount = 0

    EnterCriticalSection g_csDev

    FOR idx = 0 TO %MAX_DEVICES - 1
        IF g_Devs(idx).dwActive AND g_Devs(idx).dwHBActive THEN
            ' Capture IP/Port for SendStop BEFORE clearing dwHBActive
            stopIPs(idx)   = g_Devs(idx).dwIP
            stopPorts(idx) = g_Devs(idx).dwPort
            g_Devs(idx).dwRunning  = 0
            g_Devs(idx).dwHBActive = 0
            g_Devs(idx).dwStatus   = %STS_IDLE
            logLines(logCount) = "[HB] dwHBActive=0 for dev #" & _
                                  TRIM$(STR$(idx)) & _
                                  " - heartbeat STOPPED (stop all)"
            INCR logCount
            ' FIX (GROK-3): capture thread handle for post-CS wait,
            ' and clear it from g_Devs so a concurrent StartStream
            ' sees hAudioThread=0 (its precondition is dwHBActive only,
            ' but clearing the handle is still good hygiene).
            waitThreads(waitCount) = g_Devs(idx).hAudioThread
            waitDevs(waitCount)    = idx
            INCR waitCount
            g_Devs(idx).hAudioThread = 0
        ELSE
            stopIPs(idx) = 0
        END IF
        ' Read fAudioFile under CS and zero it
        fNums(idx) = g_Devs(idx).fAudioFile
        g_Devs(idx).fAudioFile = 0
    NEXT idx

    LeaveCriticalSection g_csDev

    ' Emit deferred log lines OUTSIDE the critical section.
    FOR idx = 0 TO logCount - 1
        AddLog logLines(idx)
    NEXT idx

    ' Send explicit CMD_STOP to each device OUTSIDE CS (UDP SEND can block)
    FOR idx = 0 TO %MAX_DEVICES - 1
        IF stopIPs(idx) THEN
            SendStop stopIPs(idx), stopPorts(idx)
        END IF
    NEXT idx

    ' FIX (UDP stop crash): Do NOT close audio sockets here. Each AudioThread
    ' OWNS its socket and closes it in its own cleanup section. Closing from
    ' this thread while an AudioThread is blocked in UDP RECV races on PB's
    ' process-global file-number table (close frees the slot while RECV still
    ' references it -> use-after-free -> instant crash, seen in UDP mode).
    ' Threads see dwRunning=0 within their RECV timeout (<=500ms UDP, <=2s TCP)
    ' and exit cleanly. (fNums() captured above only to clear fAudioFile slots.)

    ' FIX (GROK-3): WAIT for each AudioThread to actually exit before
    ' returning. Same poll-until-exit pattern as StopStream (up to 30s per
    ' thread). Without this, the "Stop All" button returns immediately
    ' while the threads are still alive, and a subsequent Start click would
    ' create duplicate AudioThreads on the same port. With this wait, the
    ' UI stays responsive (button pressed) until all threads are gone, and
    ' StartStream's precondition (dwHBActive=0 + hAudioThread=0) is safe.
    FOR idx = 0 TO waitCount - 1
        LOCAL waitH AS DWORD
        LOCAL waitDev AS LONG
        LOCAL waitMs AS LONG
        LOCAL waitRes AS LONG
        LOCAL lRes AS LONG
        waitH   = waitThreads(idx)
        waitDev = waitDevs(idx)
        IF waitH = 0 THEN ITERATE FOR
        waitMs = 0
        DO
            waitRes = WaitForSingleObject(waitH, 1000)
            ' FIX (AUDIT-WAITFAILED): exit on WAIT_FAILED too (invalid
            ' handle returns instantly - old code burned 30s at 100% CPU).
            IF waitRes = %WAIT_OBJECT_0 OR waitRes = %WAIT_FAILED THEN EXIT DO
            waitMs = waitMs + 1000
            IF waitMs >= 30000 THEN
                ' FIX (AUDIT-W2): orphan thread still alive. Restore
                ' hAudioThread (cleared at line ~3657) so StartStream's
                ' guard (hAudioThread <> 0) refuses to create a duplicate.
                ' Do NOT THREAD CLOSE the handle - keep it open so a later
                ' StopStream / WaitForAllThreads can still wait on it.
                EnterCriticalSection g_csDev
                g_Devs(waitDev).hAudioThread = waitH
                LeaveCriticalSection g_csDev
                AddLog "[AUD] dev #" & TRIM$(STR$(waitDev)) & _
                       ": StopAllStreams: AudioThread did not exit in 30s - " & _
                       "orphan thread will self-exit (handle preserved, StartStream blocked)"
                ' Skip THREAD CLOSE below - handle kept open as orphan.
                ITERATE FOR
            END IF
        LOOP
        THREAD CLOSE waitH TO lRes
    NEXT idx

    ' FIX (AUDIT-W2-FIX): reap orphan handles left by a PRIOR StopStream
    ' that timed out (dwHBActive=0 but hAudioThread<>0). Those orphans
    ' were skipped by the main loop above (which only collected
    ' dwHBActive<>0 devices). By now they have had time to self-exit
    ' (dwRunning was cleared in the prior StopStream). Wait briefly +
    ' close the handle so StartStream can restart the device.
    FOR idx = 0 TO %MAX_DEVICES - 1
        LOCAL reapH AS DWORD
        EnterCriticalSection g_csDev
        IF g_Devs(idx).dwHBActive = 0 AND g_Devs(idx).hAudioThread <> 0 THEN
            reapH = g_Devs(idx).hAudioThread
            g_Devs(idx).hAudioThread = 0
        ELSE
            reapH = 0
        END IF
        LeaveCriticalSection g_csDev
        IF reapH THEN
            WaitForSingleObject reapH, 5000
            THREAD CLOSE reapH TO lRes
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                   ": StopAllStreams: reaped orphan AudioThread handle (now restartable)"
        END IF
    NEXT idx
END SUB

' WaitForAllThreads - Wait for all worker threads to finish.
'
' FIX (AUDIT-H11): previously used WaitForSingleObject with a fixed 3s
' (audio) / 6s (heartbeat) timeout. If the wait timed out (AudioThread mid
' TCP reconnect = up to 3s per attempt; HB thread inside UDP SEND), the
' code proceeded to THREAD CLOSE and then PBMAIN called
' DeleteCriticalSection g_csDev while the thread was still alive ->
' use-after-free -> crash.
'
' New approach: poll WaitForSingleObject(hThread, 1000) in a loop until
' WAIT_OBJECT_0, with a hard ceiling of 30 s. If exceeded, log a diagnostic
' but DO NOT proceed past this function until the wait succeeds (or 30 s
' elapses with a final warning). The caller may then DeleteCriticalSection
' with much lower risk of orphaned CS access.
SUB WaitForAllThreads()
    LOCAL idx AS LONG
    LOCAL hThread AS DWORD
    LOCAL lRes AS LONG
    LOCAL waitRes AS LONG
    LOCAL totalMs AS LONG

    ' Wait for audio threads - read handles under CS
    FOR idx = 0 TO %MAX_DEVICES - 1
        EnterCriticalSection g_csDev
        hThread = g_Devs(idx).hAudioThread
        g_Devs(idx).hAudioThread = 0
        LeaveCriticalSection g_csDev

        IF hThread THEN
            totalMs = 0
            DO
                waitRes = WaitForSingleObject(hThread, 1000)
                ' FIX (AUDIT-WAITFAILED): exit on WAIT_FAILED too (invalid
                ' handle returns instantly - old code burned 30s at 100% CPU).
                IF waitRes = %WAIT_OBJECT_0 OR waitRes = %WAIT_FAILED THEN EXIT DO
                totalMs = totalMs + 1000
                IF totalMs >= 30000 THEN
                    AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                           ": thread did not exit in 30s - proceeding (CS may be unsafe)"
                    EXIT DO
                END IF
            LOOP
            THREAD CLOSE hThread TO lRes
        END IF
    NEXT idx

    ' FIX (L40 + AUDIT-H11): same poll-until-exit pattern for the heartbeat
    ' thread. SLEEP %HB_INTERVAL (3000ms) inside HB thread + processing
    ' time can exceed the previous 6s timeout; if WaitForAllThreads timed
    ' out while HB was still inside EnterCriticalSection g_csDev, the
    ' subsequent DeleteCriticalSection at exit was a use-after-free.
    IF g_hHbTh THEN
        totalMs = 0
        DO
            waitRes = WaitForSingleObject(g_hHbTh, 1000)
            ' FIX (AUDIT-WAITFAILED): exit on WAIT_FAILED too (invalid
            ' handle returns instantly - old code burned 30s at 100% CPU).
            IF waitRes = %WAIT_OBJECT_0 OR waitRes = %WAIT_FAILED THEN EXIT DO
            totalMs = totalMs + 1000
            IF totalMs >= 30000 THEN
                AddLog "[HB] thread did not exit in 30s - proceeding (CS may be unsafe)"
                EXIT DO
            END IF
        LOOP
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
' NOTE: PowerBASIC SEEK is 1-based (first byte = position 1), so we add 1
' to the 0-based WAV offsets.
SUB UpdateWavHeader()
    IF g_hDumpFile = 0 THEN EXIT SUB

    ' Update data chunk size (0-based offset 40 -> 1-based position 41)
    SEEK #g_hDumpFile, 41
    PUT$ #g_hDumpFile, MKL$(g_dumpDataSize)

    ' Update RIFF size (0-based offset 4 -> 1-based position 5)
    ' RIFF size = 36 + data size
    SEEK #g_hDumpFile, 5
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
        ' FIX (Partial#14): g_bDumping write under g_csDump for memory-order
        ' consistency with AudioThread reads. LONG writes are atomic on x86,
        ' but a single CS covers the flag + g_hDumpFile close as one atomic
        ' transition - no window where AudioThread sees g_bDumping=0 but
        ' g_hDumpFile still open and writes to a closing handle.
        EnterCriticalSection g_csDump
        g_bDumping = 0
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
        ' FIX (M38): prefix with EXE.PATH$ so dump files land next to the
        ' exe regardless of the current working directory (shortcuts with a
        ' different "Start in" folder would otherwise put them somewhere
        ' unpredictable).
        g_dumpBaseName = EXE.PATH$ & "dump_" & _
                RIGHT$("0" & TRIM$(STR$(stDump.wHour)), 2) & _
                RIGHT$("0" & TRIM$(STR$(stDump.wMinute)), 2) & _
                RIGHT$("0" & TRIM$(STR$(stDump.wSecond)), 2)
        g_dumpFileIdx = 0
        g_dumpWavReady = 0
        g_dumpCodec = 0
        g_dumpDataSize = 0
        g_dumpSeqCounter = 0
        ' FIX (GROK-9): record the currently-selected device's IP so the
        ' dump write paths in AudioThread can filter by device. Without
        ' this, 2+ concurrent AudioThreads would all write to the single
        ' global g_hDumpFile -> byte-interleaved PCM -> unplayable garbage.
        ' Now only the selected device's PCM is dumped; other devices are
        ' ignored. If no device is selected, g_dumpIp=0 and ALL devices
        ' dump (legacy behavior, useful for single-device setups).
        LOCAL dumpSelIdx AS LONG
        dumpSelIdx = GetSelectedDeviceIdx()
        IF dumpSelIdx >= 0 THEN
            EnterCriticalSection g_csDev
            g_dumpIp = g_Devs(dumpSelIdx).dwIP
            LeaveCriticalSection g_csDev
            AddLog "WAV dump: filtering to selected dev #" & TRIM$(STR$(dumpSelIdx)) & _
                   " (IP=" & FormatIP(g_dumpIp) & ")"
        ELSE
            g_dumpIp = 0
            AddLog "WAV dump: no device selected - dumping ALL active streams (may mix PCM)"
        END IF
        ' Will open file on first packet (need format info from packet header).
        ' FIX (Partial#14): g_bDumping write under CS so AudioThread's
        ' first-packet check (also under CS) sees a consistent flag+state.
        EnterCriticalSection g_csDump
        g_bDumping = 1
        LeaveCriticalSection g_csDump
        CONTROL SET TEXT g_hDlg, %IDC_BTN_DUMP, "STOP DUMP"
        AddLog "WAV dump: waiting for first packet to determine format..."
    END IF
END SUB

' RefreshUI - Dynamically add/remove/update ListView items + StatusBar
'   - New devices: insert row
'   - Gone devices: remove row (log it)
'   - Existing devices: update sub-items in-place
'
' FIX (GROK-7): previously held g_csDev across ~250 SendMessage calls
' (Phase 1 deletes + Phase 2 per-device column updates), blocking
' AudioThread/HB on every packet/stats update -> jitter / UI freezes
' with 16 devices. Now we snapshot g_Devs[] into a local array under a
' SINGLE brief CS enter/leave, then do ALL ListView work outside CS.
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
    ' FIX (GROK-7): local snapshot of g_Devs for CS-free ListView updates.
    ' DeviceInfo contains only fixed-size fields (ASCIIZ arrays, DWORDs,
    ' LONGs) so a struct assignment is a flat memcpy - safe without CS
    ' on the read side after the snapshot.
    DIM snap(%MAX_DEVICES - 1) AS LOCAL DeviceInfo

    goneCount = 0

    ' Save current selection (device array index)
    selIdx = GetSelectedDevIdx

    ' Disable redraw to prevent flicker
    CONTROL HANDLE g_hDlg, %IDC_LISTVIEW TO hLV
    IF hLV = 0 THEN EXIT SUB    ' guard: ListView not yet created
    SendMessage hLV, %WM_SETREDRAW, 0, 0

    ' FIX (GROK-7): snapshot g_Devs[] under a SINGLE brief CS, then do ALL
    ' ListView work outside CS. Previously the CS was held across ~250
    ' SendMessage calls, blocking AudioThread/HB on every packet.
    EnterCriticalSection g_csDev
    FOR i = 0 TO %MAX_DEVICES - 1
        snap(i) = g_Devs(i)
    NEXT i
    LeaveCriticalSection g_csDev

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
        ELSEIF snap(devIdx).dwActive = 0 THEN
            ' Device gone - collect MAC, remove row (log AFTER leaving CS)
            goneMacs(goneCount) = TRIM$(snap(devIdx).sMac)
            INCR goneCount
            SendMessage hLV, %LVM_DELETEITEM, lvIdx, 0
        END IF
    NEXT lvIdx

    ' ---- Phase 2: Update existing rows and insert new ones ----
    cnt = 0
    strmCnt = 0

    FOR i = 0 TO %MAX_DEVICES - 1
        IF snap(i).dwActive = 0 THEN ITERATE FOR

        INCR cnt
        IF snap(i).dwHBActive THEN INCR strmCnt

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
            ' Show "hostname (MAC)" — both visible
            IF LEN(TRIM$(snap(i).sHostname)) > 0 THEN
                szText = TRIM$(snap(i).sHostname) & " (" & TRIM$(snap(i).sMac) & ")"
            ELSE
                szText = TRIM$(snap(i).sMac)
            END IF
            lvi.mask     = %LVIF_TEXT OR %LVIF_PARAM
            lvi.iItem    = SendMessage(hLV, %LVM_GETITEMCOUNT, 0, 0)   ' append at end
            lvi.iSubItem = 0
            lvi.pszText  = VARPTR(szText)
            lvi.lParam   = i           ' store device array index
            lvIdx = SendMessage(hLV, %LVM_INSERTITEM, 0, VARPTR(lvi))
        END IF

        ' Update col 0 for existing devices (hostname may have arrived in later INFO)
        IF LEN(TRIM$(snap(i).sHostname)) > 0 THEN
            szText = TRIM$(snap(i).sHostname) & " (" & TRIM$(snap(i).sMac) & ")"
        ELSE
            szText = TRIM$(snap(i).sMac)
        END IF
        lvi.mask     = %LVIF_TEXT
        lvi.iItem    = lvIdx
        lvi.iSubItem = %LV_COL_MAC
        lvi.pszText  = VARPTR(szText)
        SendMessage hLV, %LVM_SETITEMTEXT, lvIdx, VARPTR(lvi)

        ' Col 1 - IP:Port
        szText = FormatIP(snap(i).dwIP) & ":" & TRIM$(STR$(snap(i).dwPort))
        lvi.mask     = %LVIF_TEXT
        lvi.iItem    = lvIdx
        lvi.iSubItem = %LV_COL_IP
        lvi.pszText  = VARPTR(szText)
        SendMessage hLV, %LVM_SETITEMTEXT, lvIdx, VARPTR(lvi)

        ' Col 2 - Status
        SELECT CASE snap(i).dwStatus
            CASE %STS_STREAM: sStatus = "Streaming"
            CASE %STS_ERROR:  sStatus = "Error"
            CASE ELSE:        sStatus = "Idle"
        END SELECT
        szText = sStatus
        lvi.iSubItem = %LV_COL_STATUS
        lvi.pszText  = VARPTR(szText)
        SendMessage hLV, %LVM_SETITEMTEXT, lvIdx, VARPTR(lvi)

        ' Col 3 - Sample Rate
        szText = TRIM$(STR$(snap(i).dwSmpRate)) & " Hz"
        lvi.iSubItem = %LV_COL_RATE
        lvi.pszText  = VARPTR(szText)
        SendMessage hLV, %LVM_SETITEMTEXT, lvIdx, VARPTR(lvi)

        ' Col 4 - Bits
        IF snap(i).dwBits > 0 THEN
            szText = TRIM$(STR$(snap(i).dwBits)) & "-bit"
        ELSE
            szText = "-"
        END IF
        lvi.iSubItem = %LV_COL_BITS
        lvi.pszText  = VARPTR(szText)
        SendMessage hLV, %LVM_SETITEMTEXT, lvIdx, VARPTR(lvi)

        ' Col 5 - Channels
        SELECT CASE snap(i).dwChannels
            CASE 1:  szText = "mono"
            CASE 2:  szText = "stereo"
            CASE ELSE: szText = TRIM$(STR$(snap(i).dwChannels))
        END SELECT
        lvi.iSubItem = %LV_COL_CH
        lvi.pszText  = VARPTR(szText)
        SendMessage hLV, %LVM_SETITEMTEXT, lvIdx, VARPTR(lvi)

        ' Col 6 - Codec (with transport suffix: ADPCM/UDP, PCM/TCP, etc.)
        SELECT CASE snap(i).dwCodec
            CASE %CODEC_ID:     szText = "ADPCM"
            CASE %CODEC_ID_PCM: szText = "PCM"
            CASE ELSE:          szText = TRIM$(STR$(snap(i).dwCodec))
        END SELECT
        SELECT CASE snap(i).dwTransport
            CASE 1: szText = szText & "/TCP"
            CASE 0: szText = szText & "/UDP"
            CASE ELSE: szText = szText & "/NONE"
        END SELECT
        lvi.iSubItem = %LV_COL_CODEC
        lvi.pszText  = VARPTR(szText)
        SendMessage hLV, %LVM_SETITEMTEXT, lvIdx, VARPTR(lvi)

        ' Col 7 - RSSI
        szText = STR$(snap(i).dwRSSI) & " dBm"
        lvi.iSubItem = %LV_COL_RSSI
        lvi.pszText  = VARPTR(szText)
        SendMessage hLV, %LVM_SETITEMTEXT, lvIdx, VARPTR(lvi)

        ' Col 8 - Heap (FIX GROK-L2: was mislabeled 'Col 4')
        IF snap(i).dwFreeHeap >= 1024 THEN
            sHeap = LTRIM$(STR$(snap(i).dwFreeHeap \ 1024)) & "KB"
        ELSE
            sHeap = LTRIM$(STR$(snap(i).dwFreeHeap)) & "B"
        END IF
        szText = sHeap
        lvi.iSubItem = %LV_COL_HEAP
        lvi.pszText  = VARPTR(szText)
        SendMessage hLV, %LVM_SETITEMTEXT, lvIdx, VARPTR(lvi)

        ' Col 9 - Firmware (FIX GROK-L2: was mislabeled 'Col 5')
        szText = TRIM$(snap(i).sFirmware)
        lvi.iSubItem = %LV_COL_FW
        lvi.pszText  = VARPTR(szText)
        SendMessage hLV, %LVM_SETITEMTEXT, lvIdx, VARPTR(lvi)

        ' Col 10 - Packets (FIX GROK-L2: was mislabeled 'Col 6')
        szText = STR$(snap(i).dwPktRecv)
        lvi.iSubItem = %LV_COL_PKTS
        lvi.pszText  = VARPTR(szText)
        SendMessage hLV, %LVM_SETITEMTEXT, lvIdx, VARPTR(lvi)

        ' Col 11 - Lost (FIX GROK-L2: was mislabeled 'Col 7')
        szText = STR$(snap(i).dwPktLost)
        lvi.iSubItem = %LV_COL_LOST
        lvi.pszText  = VARPTR(szText)
        SendMessage hLV, %LVM_SETITEMTEXT, lvIdx, VARPTR(lvi)

        ' Col 12 - Duration (FIX GROK-L2: was mislabeled 'Col 8')
        IF snap(i).dwHBActive THEN
            tick = GetTickCount()
            totalSecs = (tick - snap(i).dwStreamStart) \ 1000
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

    ' FIX (GROK-7): CS was already released after the snapshot above.
    ' All ListView updates in Phase 1 and Phase 2 are now CS-free.

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

    ' Update StatusBar (5 parts)
    sLine = " EASSP Server"
    CONTROL SEND g_hDlg, %IDC_STATUSBAR, %SB_SETTEXT, 0, STRPTR(sLine)
    sLine = " Devices:" & STR$(cnt)
    CONTROL SEND g_hDlg, %IDC_STATUSBAR, %SB_SETTEXT, 1, STRPTR(sLine)
    sLine = " Streaming:" & STR$(strmCnt)
    CONTROL SEND g_hDlg, %IDC_STATUSBAR, %SB_SETTEXT, 2, STRPTR(sLine)
    sLine = " UDP:3950"
    CONTROL SEND g_hDlg, %IDC_STATUSBAR, %SB_SETTEXT, 3, STRPTR(sLine)
    ' Part 4: selected output device name
    LOCAL sbDevIdx AS LONG
    COMBOBOX GET SELECT g_hDlg, %IDC_COMBO_DEVICE TO sbDevIdx
    IF sbDevIdx <= 1 THEN
        sLine = " Output: Default"
    ELSE
        LOCAL devText AS STRING
        COMBOBOX GET TEXT g_hDlg, %IDC_COMBO_DEVICE, sbDevIdx TO devText
        sLine = " Output: " & devText
    END IF
    CONTROL SEND g_hDlg, %IDC_STATUSBAR, %SB_SETTEXT, 4, STRPTR(sLine)

    ' Update button enable/disable state
    UpdateButtonStates
END SUB

' ============================================================================
'  Main Dialog Callback
' ============================================================================
CALLBACK FUNCTION MainDlgProc()

    SELECT CASE CB.MSG

        CASE %WM_INITDIALOG
            ' Set the dialog icon (caption bar + Alt+Tab task list) from the
            ' embedded #RESOURCE ICON, 100. Per PowerBASIC docs, DIALOG SET
            ' ICON takes the resource ID as "#<id>" (integral) or the text
            ' name, and assigns BOTH the small (caption) and large (Alt+Tab)
            ' icons in one call. No LoadImage/WM_SETICON needed — that is the
            ' raw SDK way; this is the DDT way.
            DIALOG SET ICON CB.HNDL, "#100"

            ' Set timer for UI refresh
            SetTimer CB.HNDL, %IDT_REFRESH, %REFRESH_MS, %NULL
            ' Initial layout
            ResizeControls
            ' Populate StatusBar immediately so it isn't blank for the first
            ' %REFRESH_MS (2s) until the timer's first tick. Safe: the bar and
            ' its 5 parts are created in WinMain before DIALOG SHOW MODAL, and
            ' RefreshUI has an hLV=0 guard.
            RefreshUI
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
            ' Distinguish timers by ID (CB.WPARAM = timer ID).
            IF CB.WPARAM = %IDT_DEVCHANGE THEN
                ' Debounce timer fired: all WM_DEVICECHANGE / WM_POWERBROADCAST
                ' events have settled (no new device changes for 2 s). Now it's
                ' safe to reopen WaveOut — all audio devices (HDMI, USB, etc.)
                ' have finished re-initializing. Flag reopen for all running
                ' streams.
                KillTimer CB.HNDL, %IDT_DEVCHANGE
                AddLog "[DEV] Device change debounce complete - flagging WaveOut reopen"
                LOCAL dt_rc AS LONG
                FOR dt_rc = 0 TO %MAX_DEVICES - 1
                    IF g_Devs(dt_rc).dwRunning THEN
                        ' FIX (H15): protect bReopenWaveOut write with CS

                        EnterCriticalSection g_csDev

                        g_Devs(dt_rc).bReopenWaveOut = 1

                        LeaveCriticalSection g_csDev
                    END IF
                NEXT dt_rc
                FUNCTION = 1
                EXIT FUNCTION
            END IF
            ' Default: UI refresh timer (%IDT_REFRESH)
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

                CASE %IDC_COMBO_DEVICE
                    IF CB.CTLMSG = %CBN_SELCHANGE THEN
                        ' Change output device for the SELECTED device in ListView.
                        ' If no device selected, change for all (global default).
                        LOCAL selIdx AS LONG
                        LOCAL selDevIdx AS LONG
                        selDevIdx = GetSelectedDeviceIdx()
                        COMBOBOX GET SELECT g_hDlg, %IDC_COMBO_DEVICE TO selIdx
                        IF selIdx <= 1 THEN
                            IF selDevIdx >= 0 THEN
                                ' FIX (BUG#3): use CS-safe helper
                                SetDeviceWaveOutput selDevIdx, -1
                            ELSE
                                AddLog "Select a device in ListView first, then choose output"
                            END IF
                        ELSE
                            IF selDevIdx >= 0 THEN
                                ' FIX (BUG#3): use CS-safe helper
                                SetDeviceWaveOutput selDevIdx, selIdx - 2
                            ELSE
                                AddLog "Select a device in ListView first, then choose output"
                            END IF
                        END IF
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
                ' Right-click on ListView → show context menu
                IF @pnmh.hwndFrom = hLV AND @pnmh.code = %NM_RCLICK THEN
                    LOCAL hPopup AS DWORD
                    LOCAL pt AS POINTAPI
                    LOCAL i AS LONG
                    LOCAL nCount AS LONG
                    LOCAL nChecked AS LONG
                    LOCAL nStreaming AS LONG

                    hPopup = CreatePopupMenu()
                    IF hPopup THEN
                        ' Count checked + streaming for menu state
                        ' FIX (GROK-12): snapshot dwHBActive + dwWaveDevice
                        ' under CS to avoid cosmetic races with AudioThread
                        ' (which writes these under CS). Without this, the
                        ' context menu could show a stale checkmark or
                        ' grayed-out state for one menu invocation.
                        LOCAL ctxDevIdx AS LONG
                        DIM ctxHBActive(%MAX_DEVICES - 1) AS LOCAL LONG
                        DIM ctxWaveDev(%MAX_DEVICES - 1) AS LOCAL LONG
                        EnterCriticalSection g_csDev
                        FOR ctxDevIdx = 0 TO %MAX_DEVICES - 1
                            ctxHBActive(ctxDevIdx) = g_Devs(ctxDevIdx).dwHBActive
                            ctxWaveDev(ctxDevIdx)  = g_Devs(ctxDevIdx).dwWaveDevice
                        NEXT ctxDevIdx
                        LeaveCriticalSection g_csDev

                        nCount = SendMessage(hLV, %LVM_GETITEMCOUNT, 0, 0)
                        FOR i = 0 TO nCount - 1
                            IF IsItemChecked(hLV, i) THEN INCR nChecked
                            LOCAL lviTmp AS LV_ITM
                            lviTmp.mask = %LVIF_PARAM
                            lviTmp.iItem = i
                            lviTmp.iSubItem = 0
                            SendMessage hLV, %LVM_GETITEM, 0, VARPTR(lviTmp)
                            IF lviTmp.lParam >= 0 AND lviTmp.lParam < %MAX_DEVICES THEN
                                IF ctxHBActive(lviTmp.lParam) THEN INCR nStreaming
                            END IF
                        NEXT i

                        ' Build menu
                        AppendMenu hPopup, %MF_STRING, %IDM_CTX_START, "Start Stream"
                        AppendMenu hPopup, %MF_STRING, %IDM_CTX_STOP,  "Stop Stream"

                        ' Output Device submenu (for the selected device)
                        LOCAL hSubOutput AS DWORD
                        LOCAL selDev AS LONG
                        LOCAL numDevs AS LONG
                        LOCAL di AS LONG
                        LOCAL capsOut AS WAVEOUTCAPS
                        LOCAL sDevName AS STRING

                        hSubOutput = CreatePopupMenu()
                        selDev = GetSelectedDeviceIdx()

                        ' "Default (WAVE_MAPPER)" - ID = &H2100
                        AppendMenu hSubOutput, %MF_STRING, &H2100, "Default (WAVE_MAPPER)"
                        IF selDev >= 0 AND ctxWaveDev(selDev) = -1 THEN
                            CheckMenuItem hSubOutput, &H2100, %MF_CHECKED
                        END IF

                        ' Enumerate WaveOut devices
                        numDevs = waveOutGetNumDevs()
                        FOR di = 0 TO numDevs - 1
                            IF waveOutGetDevCaps(di, capsOut, SIZEOF(capsOut)) = 0 THEN
                                sDevName = EXTRACT$(capsOut.szPname, CHR$(0))
                                IF LEN(sDevName) = 0 THEN sDevName = "Device " & TRIM$(STR$(di))
                                ' ID = &H2101 + di (device 0 = &H2101, 1 = &H2102, ...)
                                AppendMenu hSubOutput, %MF_STRING, &H2101 + di, _
                                    TRIM$(STR$(di)) & ": " & sDevName
                                IF selDev >= 0 AND ctxWaveDev(selDev) = di THEN
                                    CheckMenuItem hSubOutput, &H2101 + di, %MF_CHECKED
                                END IF
                            END IF
                        NEXT di

                        AppendMenu hPopup, %MF_SEPARATOR, 0, BYVAL %NULL
                        AppendMenu hPopup, %MF_POPUP OR %MF_STRING, hSubOutput, "Output Device"
                        AppendMenu hPopup, %MF_SEPARATOR, 0, BYVAL %NULL
                        AppendMenu hPopup, %MF_STRING, %IDM_CTX_SELECTALL, "Select All"
                        AppendMenu hPopup, %MF_STRING, %IDM_CTX_CLEARALL,  "Clear All"
                        AppendMenu hPopup, %MF_SEPARATOR, 0, BYVAL %NULL
                        AppendMenu hPopup, %MF_STRING, %IDM_CTX_STOPALL,  "Stop All Streams"

                        ' Enable/disable based on state
                        IF nChecked = 0 THEN
                            EnableMenuItem hPopup, %IDM_CTX_START, %MF_GRAYED
                            EnableMenuItem hPopup, %IDM_CTX_STOP,  %MF_GRAYED
                        END IF
                        IF nStreaming = 0 THEN
                            EnableMenuItem hPopup, %IDM_CTX_STOPALL, %MF_GRAYED
                        END IF
                        IF nCount = 0 THEN
                            EnableMenuItem hPopup, %IDM_CTX_SELECTALL, %MF_GRAYED
                            EnableMenuItem hPopup, %IDM_CTX_CLEARALL,  %MF_GRAYED
                        END IF
                        IF selDev < 0 THEN
                            ' FIX (Partial#9): EnableMenuItem by command-ID
                            ' (hSubOutput handle) works on most Windows versions
                            ' because AppendMenu(MF_POPUP) stores the HMENU as the
                            ' command ID, but this is undocumented and fragile.
                            ' Switch to MF_BYPOSITION with a position lookup.
                            LOCAL subOutPos AS LONG
                            LOCAL ctxPos AS LONG
                            subOutPos = -1
                            FOR ctxPos = 0 TO GetMenuItemCount(hPopup) - 1
                                IF GetSubMenu(hPopup, ctxPos) = hSubOutput THEN
                                    subOutPos = ctxPos
                                    EXIT FOR
                                END IF
                            NEXT ctxPos
                            IF subOutPos >= 0 THEN
                                EnableMenuItem hPopup, subOutPos, %MF_BYPOSITION OR %MF_GRAYED
                            END IF
                        END IF

                        ' Show menu at cursor position
                        GetCursorPos pt
                        LOCAL ctxCmd AS LONG
                        ctxCmd = TrackPopupMenu(hPopup, %TPM_LEFTALIGN OR _
                                       %TPM_TOPALIGN OR %TPM_RETURNCMD OR _
                                       %TPM_RIGHTBUTTON, _
                                       pt.x, pt.y, 0, CB.HNDL, BYVAL %NULL)
                        DestroyMenu hPopup
                        ' FIX (BUG#2): removed second "DestroyMenu hSubOutput".
                        ' Per MSDN, DestroyMenu is recursive: it destroys the menu
                        ' AND all its submenus. hSubOutput was attached to hPopup
                        ' via AppendMenu(MF_POPUP), so DestroyMenu hPopup already
                        ' freed it. Calling DestroyMenu hSubOutput again was a
                        ' use-after-free (usually silent, occasionally crashes on
                        ' heap introspection). hSubOutput is now a dangling handle
                        ' - do not touch it.

                        ' Handle selected menu item
                        SELECT CASE ctxCmd
                            CASE %IDM_CTX_START
                                StartCheckedStreams
                                UpdateButtonStates
                            CASE %IDM_CTX_STOP
                                StopCheckedStreams
                                UpdateButtonStates
                            CASE %IDM_CTX_STOPALL
                                StopAllStreams
                                UpdateButtonStates
                            CASE %IDM_CTX_SELECTALL
                                ' Check all items in ListView
                                FOR i = 0 TO nCount - 1
                                    SetItemChecked hLV, i, 1
                                NEXT i
                                UpdateButtonStates
                            CASE %IDM_CTX_CLEARALL
                                ' Uncheck all items in ListView
                                FOR i = 0 TO nCount - 1
                                    SetItemChecked hLV, i, 0
                                NEXT i
                                UpdateButtonStates
                            CASE &H2100
                                ' "Default (WAVE_MAPPER)" selected for the focused device
                                IF selDev >= 0 THEN
                                    ' FIX (BUG#3): use CS-safe helper
                                    SetDeviceWaveOutput selDev, -1
                                END IF
                            CASE &H2101 TO &H21FF
                                ' Specific device ID = ctxCmd - &H2101
                                IF selDev >= 0 THEN
                                    ' FIX (BUG#3): use CS-safe helper
                                    SetDeviceWaveOutput selDev, ctxCmd - &H2101
                                END IF
                        END SELECT
                        FUNCTION = 1
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
                ' FIX (AUDIT-INFO2): per PBWin.txt L81645, a UDP NOTIFY
                ' is auto re-armed by the next UDP RECV (which DiscoveryProc
                ' calls above). It is NOT 'persistent' in the sense of firing
                ' continuously - it fires once per RECV. But re-issuing the
                ' UDP NOTIFY statement manually here would register a SECOND
                ' notification request and can leak internal handles, so we
                ' correctly do nothing. The single NOTIFY in InitDiscovery()
                ' plus DiscoveryProc's UDP RECV is the proper re-arm cycle.
            END IF
            FUNCTION = 1
            EXIT FUNCTION

        CASE %WM_APP_LOG
            ' FIX C5: Async log message from worker threads.
            ' wParam = byte length of the string, lParam = pointer to a
            ' NUL-terminated heap buffer allocated by AddLog.
            ' Read exactly wParam bytes (previously PEEK$ read a fixed 4096
            ' bytes, reading past the allocation), then free the heap block.
            IF CB.LPARAM THEN
                LOCAL pLogBuf AS BYTE PTR
                LOCAL nLogLen AS LONG
                LOCAL sLogLine AS STRING
                LOCAL nCurLen AS LONG
                pLogBuf = CB.LPARAM
                nLogLen = CB.WPARAM
                IF nLogLen > 0 THEN
                    sLogLine = PEEK$(pLogBuf, nLogLen)
                    CONTROL SEND g_hDlg, %IDC_LOG, %WM_GETTEXTLENGTH, 0, 0 TO nCurLen
                    IF nCurLen > 32768 THEN
                        CONTROL SET TEXT g_hDlg, %IDC_LOG, ""
                        nCurLen = 0
                    END IF
                    CONTROL SEND g_hDlg, %IDC_LOG, %EM_SETSEL, nCurLen, nCurLen
                    CONTROL SEND g_hDlg, %IDC_LOG, %EM_REPLACESEL, 0, STRPTR(sLogLine)
                END IF
                HeapFree g_hHeap, 0, pLogBuf
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

        CASE %WM_DEVICECHANGE
            ' Windows sends WM_DEVICECHANGE when audio devices are
            ' disconnected/reconnected — including HDMI audio re-init after
            ' sleep/wake. After sleep/wake, MULTIPLE devices may reinitialize
            ' (HDMI audio + USB sound card), so Windows sends several
            ' WM_DEVICECHANGE events in rapid succession.
            '
            ' Instead of immediately flagging reopen on each event (which
            ' causes duplicate reopens and races), we DEBOUNCE: (re)set a 2s
            ' timer on each event. When the timer fires (2s after the LAST
            ' WM_DEVICECHANGE), all devices have settled and we reopen WaveOut
            ' once. The timer is reset on every new event, so only the final
            ' burst end triggers the reopen.
            '
            ' wParam = DBT_DEVNODES_CHANGED (7) is the generic device tree
            ' changed notification.
            IF CB.WPARAM = 7 THEN  ' %DBT_DEVNODES_CHANGED
                ' (Re)set the debounce timer. KillTimer + SetTimer resets
                ' the countdown to 2s from now.
                KillTimer CB.HNDL, %IDT_DEVCHANGE
                SetTimer CB.HNDL, %IDT_DEVCHANGE, %DEVCHANGE_DEBOUNCE_MS, %NULL
            END IF
            FUNCTION = 1
            EXIT FUNCTION

        CASE %WM_POWERBROADCAST
            ' Windows sends WM_POWERBROADCAST on sleep/resume:
            '   PBT_APMSUSPEND (4)       — BEFORE sleep (system is suspending)
            '   PBT_APMRESUMEAUTOMATIC (18) — AFTER wake (always sent)
            '   PBT_APMRESUMESUSPEND (7)  — AFTER wake (if triggered by user input)
            '
            ' On PBT_APMSUSPEND: immediately flag WaveOut reopen. This closes
            ' the TCP socket NOW (before the network stack goes down), so when
            ' the system wakes up, the audio thread is already in the reconnect
            ' state. No need to wait for WM_DEVICECHANGE debounce.
            '
            ' On resume (18 or 7): start the debounce timer — after sleep,
            ' multiple WM_DEVICECHANGE events fire as audio devices re-init,
            ' and the debounce timer catches the last one (2s after devices
            ' settle). The TCP reconnect happens inside bReopenWaveOut handling.
            IF CB.WPARAM = 4 THEN  ' PBT_APMSUSPEND — going to sleep
                AddLog "[PWR] System suspending (sleep) - flagging WaveOut + TCP reopen"
                LOCAL ps_rc AS LONG
                FOR ps_rc = 0 TO %MAX_DEVICES - 1
                    IF g_Devs(ps_rc).dwRunning THEN
                        ' FIX (H15): protect bReopenWaveOut write with CS

                        EnterCriticalSection g_csDev

                        g_Devs(ps_rc).bReopenWaveOut = 1

                        LeaveCriticalSection g_csDev
                    END IF
                NEXT ps_rc
            ELSEIF CB.WPARAM = 18 OR CB.WPARAM = 7 THEN  ' PBT_APMRESUMEAUTOMATIC / PBT_APMRESUMESUSPEND
                AddLog "[PWR] System resumed from sleep - flagging WaveOut + TCP reopen"
                KillTimer CB.HNDL, %IDT_DEVCHANGE
                SetTimer CB.HNDL, %IDT_DEVCHANGE, %DEVCHANGE_DEBOUNCE_MS, %NULL
            END IF
            FUNCTION = 1
            EXIT FUNCTION

        CASE %WM_CLOSE
            g_bRunning = 0
            KillTimer CB.HNDL, %IDT_REFRESH
            KillTimer CB.HNDL, %IDT_DEVCHANGE

            ' ---- Save window position/size to INI ----
            LOCAL rcWnd AS RECT
            GetWindowRect CB.HNDL, rcWnd
            IF IsIconic(CB.HNDL) THEN
                ' If minimized, don't save the icon position — skip save
            ELSE
                WritePrivateProfileString "window", "x", _
                    BYCOPY TRIM$(STR$(rcWnd.nLeft)), BYCOPY g_sIniFile
                WritePrivateProfileString "window", "y", _
                    BYCOPY TRIM$(STR$(rcWnd.nTop)), BYCOPY g_sIniFile
                WritePrivateProfileString "window", "w", _
                    BYCOPY TRIM$(STR$(rcWnd.nRight - rcWnd.nLeft)), BYCOPY g_sIniFile
                WritePrivateProfileString "window", "h", _
                    BYCOPY TRIM$(STR$(rcWnd.nBottom - rcWnd.nTop)), BYCOPY g_sIniFile
            END IF

            ' ---- Save ListView column widths to INI ----
            LOCAL saveCol AS LONG, saveW AS LONG
            FOR saveCol = 0 TO %LV_COL_DUR
                CONTROL SEND g_hDlg, %IDC_LISTVIEW, %LVM_GETCOLUMNWIDTH, _
                    saveCol, 0 TO saveW
                WritePrivateProfileString "listview", _
                    "col" & TRIM$(STR$(saveCol)), _
                    BYCOPY TRIM$(STR$(saveW)), BYCOPY g_sIniFile
            NEXT saveCol

            ' Stop all streams (closes audio UDP to unblock recv)
            StopAllStreams

            ' FIX (AUDIT-H12 + AUDIT-H13): wait for all worker threads to
            ' exit BEFORE closing the dump file and the discovery socket.
            ' AudioThread writes to g_hDumpFile under g_csDump (lines
            ' ~2010-2023 / 2422-2435) and HBThread uses #g_fDiscFile in
            ' SendDiscover/SendConfigure/SendStop. Closing either from the
            ' GUI thread while the worker is inside the corresponding PB
            ' file-number operation races on PB's process-global file
            ' table -> use-after-free -> crash. WaitForAllThreads now polls
            ' until WAIT_OBJECT_0 (AUDIT-H11 fix), so it is safe to close
            ' after it returns.
            WaitForAllThreads

            ' Close dump file if open (now safe - no AudioThread is alive)
            IF g_bDumping THEN
                g_bDumping = 0
                IF g_hDumpFile THEN
                    CLOSE g_hDumpFile
                    g_hDumpFile = 0
                END IF
            END IF

            ' Close discovery socket (now safe - HBThread has exited)
            IF g_fDiscOpen THEN
                UDP CLOSE #g_fDiscFile
                g_fDiscOpen = 0
            END IF

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
                    ' Save to INI: manual device list (auto-restored on restart)
                    ' + last IP/port (pre-fills Add Device dialog next time).
                    ManualDeviceAdd sIP, targetPort
                    SaveAddDeviceDefaults sIP, sPort
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
    LOCAL sDefIP AS STRING
    LOCAL sDefPort AS STRING

    ' Pre-fill with last entered values from INI (or defaults if first run).
    sDefIP   = LoadAddDeviceDefaultIP()
    sDefPort = LoadAddDeviceDefaultPort()

    DIALOG NEW hParent, "Add Device", , , 200, 90, %WS_POPUP OR %WS_BORDER OR _
              %WS_CAPTION OR %WS_SYSMENU OR %DS_CENTER, %WS_EX_DLGMODALFRAME TO hDlg

    CONTROL ADD LABEL,  hDlg, -1, "Device IP:", 10, 12, 50, 10
    CONTROL ADD TEXTBOX, hDlg, %IDC_AD_IP, sDefIP, 65, 10, 125, 12

    CONTROL ADD LABEL,  hDlg, -1, "Port:", 10, 32, 50, 10
    CONTROL ADD TEXTBOX, hDlg, %IDC_AD_PORT, sDefPort, 65, 30, 50, 12

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

    ' ---- Single instance check (BEFORE any dialog creation) ----
    ' CreateMutex is declared in WinBase.inc as:
    '   CreateMutex(lpMutexAttributes AS SECURITY_ATTRIBUTES, BYVAL bInitialOwner AS LONG, lpName AS ASCIIZ) AS DWORD
    ' First param is BYREF. We pass a properly initialized SECURITY_ATTRIBUTES
    ' struct so the handle is inheritable if needed. (Passing BYVAL 0 here is
    ' also valid in PB - it sends a NULL pointer = default security descriptor;
    ' the old comment claiming it 'corrupts the stack' was incorrect.)
    LOCAL hMutex AS DWORD
    LOCAL sa AS SECURITY_ATTRIBUTES
    sa.nLength = SIZEOF(sa)
    sa.lpSecurityDescriptor = 0
    sa.bInheritHandle = 0
    LOCAL sMutexName AS ASCIIZ * 64
    sMutexName = "EASSP_Server_SingleInstance_Mutex"
    hMutex = CreateMutex(sa, 1, sMutexName)
    IF hMutex = 0 THEN
        MSGBOX "Failed to create mutex.", %MB_ICONERROR, "EASSP Server"
        EXIT FUNCTION
    END IF
    IF GetLastError() = %ERROR_ALREADY_EXISTS THEN
        ' Another instance is already running — bring it to front and exit.
        ' FindWindow is declared as: FindWindow(lpClassName AS ASCIIZ, lpWindowName AS ASCIIZ)
        ' Both params are BYREF ASCIIZ. Pass BYVAL 0 for lpClassName (NULL = any class),
        ' and an ASCIIZ string for the window title.
        LOCAL hExisting AS DWORD
        LOCAL sTitle AS ASCIIZ * 128
        sTitle = $APP_TITLE
        hExisting = FindWindow(BYVAL 0, sTitle)
        IF hExisting THEN
            IF IsIconic(hExisting) THEN
                ShowWindow hExisting, %SW_RESTORE
            END IF
            LOCAL dwCurThread AS DWORD, dwForeThread AS DWORD
            LOCAL hForeWnd AS DWORD
            hForeWnd = GetForegroundWindow()
            dwCurThread = GetCurrentThreadId()
            dwForeThread = GetWindowThreadProcessId(hForeWnd, BYVAL 0)
            IF dwCurThread <> dwForeThread THEN
                AttachThreadInput dwCurThread, dwForeThread, 1
                SetForegroundWindow hExisting
                AttachThreadInput dwCurThread, dwForeThread, 0
            ELSE
                SetForegroundWindow hExisting
            END IF
            IF GetForegroundWindow() <> hExisting THEN
                FlashWindow hExisting, 1
            END IF
        END IF
        CloseHandle hMutex
        EXIT FUNCTION
    END IF

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

    ' Build INI file path: same folder as .exe, filename "eassp_server.ini"
    ' EXE.PATH$ returns the .exe directory WITH trailing backslash (PowerBASIC built-in).
    g_sIniFile = EXE.PATH$ & "eassp_server.ini"

    ' Init IMA ADPCM Step Table
    InitStepTable

    ' ---- Create main dialog ----
    ' Load saved window position/size from INI (or use defaults).
    ' Compute EXACT x/y before DIALOG NEW — never pass -1, because
    ' DIALOG NEW with -1 creates the window at (0,0) first and then
    ' moves it, causing a visible flash at top-left corner.
    LOCAL dlgX AS LONG, dlgY AS LONG, dlgW AS LONG, dlgH AS LONG
    dlgX = GetPrivateProfileInt("window", "x", -1, BYCOPY g_sIniFile)
    dlgY = GetPrivateProfileInt("window", "y", -1, BYCOPY g_sIniFile)
    dlgW = GetPrivateProfileInt("window", "w", 750, BYCOPY g_sIniFile)
    dlgH = GetPrivateProfileInt("window", "h", 480, BYCOPY g_sIniFile)
    ' Clamp width/height to minimums (matching WM_GETMINMAXINFO)
    IF dlgW < 500 THEN dlgW = 750
    IF dlgH < 300 THEN dlgH = 480
    ' Clamp position to visible screen (avoid off-screen if monitor setup changed)
    LOCAL screenW AS LONG, screenH AS LONG
    screenW = GetSystemMetrics(%SM_CXSCREEN)
    screenH = GetSystemMetrics(%SM_CYSCREEN)
    ' If no saved position or off-screen: center on screen explicitly
    IF dlgX < 0 OR dlgX > screenW - 100 THEN
        dlgX = (screenW - dlgW) \ 2
    END IF
    IF dlgY < 0 OR dlgY > screenH - 100 THEN
        dlgY = (screenH - dlgH) \ 2
    END IF

    ' ---- Create main dialog (HIDDEN — no %WS_VISIBLE) ----
    ' DIALOG NEW in PowerBASIC shows the window immediately if %WS_VISIBLE
    ' is in the style. By omitting it, the window is created but not shown.
    ' We set the exact position NOW (computed above), so when DIALOG SHOW
    ' MODAL makes it visible, it appears directly at the right place —
    ' no flash at (0,0).
    DIALOG NEW PIXELS, 0, $APP_TITLE, dlgX, dlgY, dlgW, dlgH, _
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
    CONTROL ADD BUTTON, g_hDlg, %IDC_BTN_DUMP,    "DUMP",        190, 290, 90, 26
    CONTROL DISABLE g_hDlg, %IDC_BTN_START
    CONTROL DISABLE g_hDlg, %IDC_BTN_STOP
    CONTROL ENABLE  g_hDlg, %IDC_BTN_DUMP

    ' ---- WaveOut device selection ComboBox ----
    CONTROL ADD LABEL, g_hDlg, %IDC_LBL_OUTPUT, "Output:", 286, 293, 40, 12
    CONTROL ADD COMBOBOX, g_hDlg, %IDC_COMBO_DEVICE, , 328, 290, 320, 200, _
        %WS_CHILD OR %WS_VISIBLE OR %WS_TABSTOP OR _
        %CBS_DROPDOWNLIST OR %WS_VSCROLL, %WS_EX_CLIENTEDGE

    ' ---- Stop All button (right-anchored) ----
    CONTROL ADD BUTTON, g_hDlg, %IDC_BTN_STOPALL, "Stop All",    658, 290, 90, 26
    CONTROL DISABLE g_hDlg, %IDC_BTN_STOPALL

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

    ' ---- StatusBar (multi-part) ----
    CONTROL ADD STATUSBAR , g_hDlg, %IDC_STATUSBAR, "", _
        0, 0, 0, 0, _
        %WS_CHILD OR %WS_VISIBLE OR %SBARS_SIZEGRIP

    ' Define StatusBar parts (5 columns):
    '   [0] EASSP Server   [1] Devices: N   [2] Streaming: N   [3] Heap: N   [4] Output: device
    ' Parts are set up in ResizeControls (widths depend on window width).
    ' Initial dummy parts:
    DIM sbParts(0 TO 4) AS LONG
    sbParts(0) = 100 : sbParts(1) = 180 : sbParts(2) = 270 : sbParts(3) = 350 : sbParts(4) = -1
    CONTROL SEND g_hDlg, %IDC_STATUSBAR, %SB_SETPARTS, 5, VARPTR(sbParts(0))

    ' ---- Init ListView columns ----
    InitListView

    ' ---- Restore saved ListView column widths from INI ----
    LOCAL colIdx AS LONG, colW AS LONG
    FOR colIdx = 0 TO %LV_COL_DUR
        colW = GetPrivateProfileInt("listview", "col" & TRIM$(STR$(colIdx)), _
                -1, BYCOPY g_sIniFile)
        IF colW > 0 THEN
            CONTROL SEND g_hDlg, %IDC_LISTVIEW, %LVM_SETCOLUMNWIDTH, colIdx, colW
        END IF
    NEXT colIdx

    ' ---- Populate WaveOut device ComboBox ----
    PopulateDeviceCombo

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
        ' FIX (H17): check thread creation success. Without this, if HB thread
        ' fails to start, no discovery heartbeats are sent -> streams never
        ' start, devices time out after 30s with no clear cause.
        IF g_hHbTh = 0 THEN
            AddLog "FATAL: THREAD CREATE HeartbeatThread failed - discovery will not run"
        ELSE
            AddLog "EASSP Server started. Listening on UDP:3950"
        END IF
        ' Load saved manual devices from INI and send DISCOVER to each.
        ' Devices that respond will appear in the ListView automatically.
        ' Small delay so the discovery socket is fully ready before we blast
        ' DISCOVERs (HeartbeatThread just started).
        SLEEP 200
        ManualDeviceLoadAll
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

    ' FIX (BUG#13): do NOT call DeleteCriticalSection here. If
    ' WaitForAllThreads above timed out at 30s, an AudioThread (or HB thread)
    ' may still be alive and inside EnterCriticalSection g_csDev/g_csDump.
    ' Deleting the CS while a thread holds/contends it is undefined behavior
    ' (use-after-free on the CS itself). Since the process is exiting, the OS
    ' will reclaim all resources including CS objects - the leak is benign.
    ' DeleteCriticalSection g_csDev
    ' DeleteCriticalSection g_csDump   ' FIX C6

    ' FIX M5: Delete font handle (was leaking ~1KB GDI memory)
    IF hMono THEN FONT END hMono

    FUNCTION = lResult
END FUNCTION
