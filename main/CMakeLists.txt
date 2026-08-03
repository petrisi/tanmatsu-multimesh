idf_build_get_property(idf_target IDF_TARGET)
if("${idf_target}" STREQUAL "esp32p4")
list(APPEND extra_requires esp-hosted-tanmatsu)
endif()

idf_component_register(
	# UI prototype build. The verified receive stacks stay in the tree but are
	# out of this build: their adapters (meshcore_net.c / meshtastic_net.c)
	# talk to the old UI API, and bending the new UI to keep them compiling
	# would be backwards. They come back when the protocol layer is rewired to
	# a UI-independent sink.
	SRCS
		"main.c"
		"font_mono_fi.c"
		"leds.c"
		"mock_data.c"
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
