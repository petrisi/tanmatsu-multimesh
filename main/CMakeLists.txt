# Application entry point and the event loop. Nothing else belongs here.
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        mm_crypto
        mm_proto
        mm_domain
        mm_log
        mm_net
        mm_radio
        mm_ui
        vendor
        badge-bsp
        tanmatsu-wifi
        nvs_flash
        esp_lcd
)
