# The per-network stacks behind one mesh_net_t interface. Joins wire codecs,
# crypto and radio settings, and writes results into the domain model.
#
# Note what is absent: this component does not depend on mm_ui. Receive handlers
# used to call the UI directly, which is what forced them out of the build when
# the UI changed. They write to the model now; the UI reads it.
idf_component_register(
    SRCS "crypto_jobs.c" "meshcore_net.c" "meshtastic_net.c"
    INCLUDE_DIRS "include"
    REQUIRES mm_proto mm_crypto mm_domain mm_radio mm_log vendor tanmatsu-lora
)
