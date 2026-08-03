# Application entry point and the event loop. Nothing else belongs here.
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        mm_domain
        mm_net
        mm_radio
        mm_ui
        badge-bsp
        tanmatsu-wifi
        nvs_flash
        esp_lcd
)
