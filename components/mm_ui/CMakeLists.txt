# Presentation: the framebuffer, the message view, overlays, the custom font and
# the LEDs. Reads the domain model and never mutates it.
idf_component_register(
    SRCS "ui.c" "font_mono_fi.c" "leds.c"
    INCLUDE_DIRS "include"
    REQUIRES mm_domain mm_proto pax-gfx badge-bsp
    PRIV_REQUIRES esp_lcd
)
