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

# At -O2 GCC's -Wstringop-truncation fires on the strncpy(dst, src, sizeof-1)
# idiom in compile.c — a false positive (each call NUL-terminates explicitly or
# copies from a zero-initialised, length-bounded buffer). It only trips on the
# esp32c3 (RISC-V) codegen, not Xtensa. Demote to a warning so -Werror builds
# pass on every target without touching the shared parser source.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=stringop-truncation)
