idf_build_get_property(idf_target IDF_TARGET)
if("${idf_target}" STREQUAL "esp32p4")
list(APPEND extra_requires esp-hosted-tanmatsu)
endif()

idf_component_register(
	SRCS
		"main.c"
		"radio_cfg.c"
		"meshcore_wire.c"
		"meshcore_crypto.c"
		"meshcore_net.c"
		"meshtastic_wire.c"
		"meshtastic_crypto.c"
		"meshtastic_net.c"
		"ui.c"
	PRIV_REQUIRES
		esp_lcd
		fatfs
		nvs_flash
		bt
		mbedtls
		badge-bsp
		tanmatsu-wifi
		tanmatsu-lora
		${extra_requires}
	INCLUDE_DIRS
		"."
)
