# Third-party and standalone algorithm code. Nothing here is application logic.
#
# ed25519.c implements RFC 8032 because ESP-IDF 6.0's mbedtls declares
# PSA_ALG_PURE_EDDSA but ships no implementation of it.
idf_component_register(
    SRCS "ed25519.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES mbedtls
)
