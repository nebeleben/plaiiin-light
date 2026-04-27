idf_component_register(
    SRCS "mjs.c"
    INCLUDE_DIRS "include"
)

# mJS is ES5-ish and mildly permissive on warnings.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-implicit-fallthrough
    -Wno-sign-compare
    -Wno-pointer-sign
    -Wno-type-limits
    -Wno-format
)
