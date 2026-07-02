# main/CMakeLists.txt — registers the main component and its source files.
#
# Source files are listed explicitly (no GLOB) for deterministic builds and
# so that removing a file from disk doesn't silently break the build.

set(COMPONENT_SRCS
    "main.c"
    "stream_mode.c"
    "i2s_capture.c"
    "adpcm_encoder.c"
    "tpdf_dither.c"
    "udp_stream.c"
    "tcp_stream.c"
    "rawtx_stream.c"
    "wifi_sta.c"
    "svc_port.c"
    "at_cmd.c"
    "config_mgr.c"
    "battery.c"
)

set(COMPONENT_ADD_INCLUDEDIRS "include")

register_component()
