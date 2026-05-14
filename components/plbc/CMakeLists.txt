idf_component_register(
    SRCS
        "vm.c"
        "compile.c"
        "serialize.c"
    INCLUDE_DIRS "."
    REQUIRES esp_timer
)

# Per-frame work is the hot path; -O2 here adds <5 KB over the rest of the
# firmware at -Og but matters a lot for the inner dispatch loop.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
