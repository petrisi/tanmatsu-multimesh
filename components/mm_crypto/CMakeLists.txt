# Crypto for both networks: channel ciphers, and the X25519 agreement behind
# direct messages. Depends on the wire types for packet layout and on mbedtls
# (PSA) for the primitives -- nothing else. Keeping radio and UI out is what
# makes the security-critical paths reviewable in isolation.
idf_component_register(
    SRCS "meshcore_crypto.c" "meshtastic_crypto.c" "x25519.c"
    INCLUDE_DIRS "include"
    REQUIRES mm_proto
    PRIV_REQUIRES mbedtls vendor
)
