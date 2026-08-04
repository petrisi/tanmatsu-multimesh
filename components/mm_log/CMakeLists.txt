# Session logging. A leaf: everything may depend on it, it depends on nothing of
# ours, so adding a log line never introduces a layering violation.
idf_component_register(
    SRCS "session_log.c"
    INCLUDE_DIRS "include"
)
