# LoRa transport: modem settings, and later the TX queue, listen-before-talk
# and the duty-cycle gate. The only component that talks to the radio.
idf_component_register(
    SRCS "radio.c" "radio_cfg.c"
    INCLUDE_DIRS "include"
    REQUIRES tanmatsu-lora
    # tanmatsu-wifi brings up the P4<->C6 SDIO RPC that the LoRa component rides
    # on; owning that here keeps the dependency with the code that needs it.
    PRIV_REQUIRES nvs_flash tanmatsu-wifi
)
