# LoRa transport: modem settings, and later the TX queue, listen-before-talk
# and the duty-cycle gate. The only component that talks to the radio.
idf_component_register(
    SRCS "radio_cfg.c"
    INCLUDE_DIRS "include"
    REQUIRES tanmatsu-lora
    PRIV_REQUIRES nvs_flash
)
