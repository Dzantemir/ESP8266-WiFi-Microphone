' ============================================================================
'  EASSP Server - TYPE Definitions
'  PowerBASIC 10 for Windows
' ============================================================================

' ---- Device Info ----
TYPE DeviceInfo
    sMac         AS ASCIIZ * 18
    dwIP         AS DWORD
    dwPort       AS DWORD
    dwStatus     AS DWORD
    dwError      AS DWORD
    sFirmware    AS ASCIIZ * 9   ' FIX C4: was * 8 — no room for NUL terminator
    dwFreeHeap   AS DWORD
    dwRSSI       AS LONG
    dwPktsSent   AS DWORD
    dwDiscovered AS DWORD
    dwLastSeen   AS DWORD
    dwAudioPort  AS DWORD
    dwHBActive   AS DWORD
    dwChannels   AS DWORD     ' 1=mono, 2=stereo (from INFO payload)
    dwSmpRate    AS DWORD     ' Sample rate in Hz (from INFO payload, e.g. 48000)
    dwCodec      AS DWORD     ' Codec ID: 5=ADPCM, 6=PCM (from INFO payload)
    dwBits       AS DWORD     ' Bits per sample: 16 or 24 (from INFO payload)
    dwTransport  AS DWORD     ' Transport: 0=UDP, 1=TCP, 2=Raw 802.11 TX (INFO v2.1)
    sHostname    AS ASCIIZ * 25  ' v2.2: hostname from INFO payload (24 bytes + NUL)
    dwWaveDevice AS LONG       ' Per-device WaveOut ID (-1=WAVE_MAPPER, 0..N=specific)
    dwPktRecv    AS DWORD
    dwPktOOO     AS DWORD
    dwPktLost    AS DWORD
    wLastSeq     AS WORD
    dwBytesRecv  AS DWORD
    dwStreamStart AS DWORD
    dwLastPktTick AS DWORD    ' ms tick of last received audio packet (stall detection)
    hWaveOut     AS DWORD
    hAudioThread AS DWORD
    fAudioFile   AS LONG       ' file number for UDP OPEN audio
    dwRunning    AS DWORD
    dwActive     AS DWORD
    bReopenWaveOut AS LONG     ' FIX (L39): 'b' prefix is misleading - this is a LONG flag, not BYTE. Kept the name for source compat; treat as 'fReopenWaveOut'. 1 = reopen WaveOut (device change / write error)
    dwLastReopenTick AS DWORD  ' GetTickCount() of last WaveOut reopen (suppress duplicates)
END TYPE

' ---- LV Column (custom, avoids pszText type issues) ----
TYPE LV_COL
    mask AS DWORD
    fmt AS LONG
    cx AS LONG
    pszText AS DWORD       ' pointer to ASCIIZ text
    cchTextMax AS LONG
    iSubItem AS LONG
END TYPE

' ---- LV Item (custom, avoids pszText type issues) ----
TYPE LV_ITM
    mask AS DWORD
    iItem AS LONG
    iSubItem AS LONG
    state AS DWORD
    stateMask AS DWORD
    pszText AS DWORD       ' pointer to ASCIIZ text
    cchTextMax AS LONG
    iImage AS LONG
    lParam AS LONG         ' stores device array index
    iIndent AS LONG
END TYPE
