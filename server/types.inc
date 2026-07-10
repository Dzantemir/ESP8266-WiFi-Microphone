' ============================================================================
'  EASSP Server - TYPE Definitions
'  PowerBASIC 10 for Windows
'
'  NOTE on LV_ITM / LV_COL:
'  These are intentionally V1-sized (pre-IE3) subsets of the standard LVITEM /
'  LVCOLUMN from CommCtrl.inc. MSDN documents that ListView messages accept
'  the V1 size for backward compatibility - the extra fields (iImage, iOrder,
'  cxMin, etc.) are only read if the corresponding LVCF_/LVIF_ mask bit is set.
'  Layout matches LVCOLUMNA / LVITEMA byte-for-byte up to and including the
'  last field used here:
'    LV_ITM  = 40 bytes (LVITEMA V1 = 6*4 + 4 trailing LONGs = OK)
'    LV_COL  = 24 bytes (LVCOLUMNA V1 = 6*4 = OK, see %LVCOLUMNA_V1_SIZE)
'  pszText is declared as DWORD (raw pointer) instead of ASCIIZ PTR purely
'  for convenience: code only writes VARPTR(szText) into it and never
'  dereferences pszText through this struct. DWORD and ASCIIZ PTR have the
'  same binary representation in PB (32-bit pointer), so SendMessage with
'  VARPTR(lvi) is safe.
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
    ' dwWaveDevice is LONG on purpose: we assign -1 (WAVE_MAPPER) and read it
    ' back as a signed value for comparisons. When passed BYVAL to waveOutOpen
    ' (which expects uDeviceID AS DWORD), PB converts LONG(-1) to
    ' DWORD(&HFFFFFFFF) preserving the bit pattern - exactly %WAVE_MAPPER
    ' (defined as &HFFFFFFFF??? in MMSystem.inc). Do NOT change to DWORD
    ' without auditing the comparison sites (lines ~4283, ~4296 in
    ' eassp_server.bas) - they compare against -1.
    dwWaveDevice AS LONG       ' Per-device WaveOut ID (-1=WAVE_MAPPER, 0..N=specific)
    dwPktRecv    AS DWORD
    dwPktOOO     AS DWORD
    dwPktLost    AS DWORD
    wLastSeq     AS WORD
    dwBytesRecv  AS QUAD        ' FIX (BUG#R2-1): was DWORD - overflowed after ~4GB
                                ' (~12h at 48kHz/16/mono). QUAD = 64-bit, no practical
                                ' overflow. AudioThread writes via CQUD(pcmLen) which
                                ' is already 64-bit safe.
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

' ---- LV Column (custom, V1-sized, layout-compatible with LVCOLUMNA) ----
TYPE LV_COL
    mask AS DWORD
    fmt AS LONG
    cx AS LONG
    pszText AS DWORD       ' pointer to ASCIIZ text (DWORD == ASCIIZ PTR layout-wise)
    cchTextMax AS LONG
    iSubItem AS LONG
END TYPE

' ---- LV Item (custom, V1-sized, layout-compatible with LVITEMA) ----
TYPE LV_ITM
    mask AS DWORD
    iItem AS LONG
    iSubItem AS LONG
    state AS DWORD
    stateMask AS DWORD
    pszText AS DWORD       ' pointer to ASCIIZ text (DWORD == ASCIIZ PTR layout-wise)
    cchTextMax AS LONG
    iImage AS LONG
    lParam AS LONG         ' stores device array index
    iIndent AS LONG
END TYPE
