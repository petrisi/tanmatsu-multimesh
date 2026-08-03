# Application state: channels, messages, identity. Owns the model that the UI
# renders and the network stacks write into. Deliberately knows nothing about
# either protocol or about drawing.
idf_component_register(
    SRCS "model.c" "settings.c"
    INCLUDE_DIRS "include"
    REQUIRES pax-gfx
    PRIV_REQUIRES nvs_flash esp_hw_support
)
