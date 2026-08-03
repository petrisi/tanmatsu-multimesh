# Wire codecs. Pure C: no ESP-IDF, no FreeRTOS, no BSP -- which is what keeps
# every file here buildable and testable on the host.
idf_component_register(
    SRCS "meshcore_wire.c" "meshtastic_wire.c" "dedup.c" "base64.c"
    INCLUDE_DIRS "include"
)
