# Phase 35 — per-form firmware. LAMPOS_FORM comes from the environment
# (build.sh exports it before idf.py); see top-level CMakeLists.txt for why.
if(DEFINED ENV{LAMPOS_FORM} AND NOT "$ENV{LAMPOS_FORM}" STREQUAL "")
    set(LAMPOS_FORM "$ENV{LAMPOS_FORM}")
endif()
if(NOT DEFINED LAMPOS_FORM OR LAMPOS_FORM STREQUAL "")
    message(FATAL_ERROR
        "LAMPOS_FORM is not set. Export it (build.sh does this automatically) "
        "or invoke idf.py via scripts/build.sh --form <name>.")
endif()
message(STATUS "Building lampos firmware for form: ${LAMPOS_FORM}")

# Run the generator before compilation so the registry + per-effect param
# helpers exist in build/generated/ when the compiler reaches them.
set(LAMPOS_HC_ROOT    "${CMAKE_CURRENT_SOURCE_DIR}/../hardcoded")
set(LAMPOS_HC_FORM    "${LAMPOS_HC_ROOT}/${LAMPOS_FORM}")
set(LAMPOS_GEN_DIR    "${CMAKE_BINARY_DIR}/generated")
set(LAMPOS_GEN_PY     "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/gen_hardcoded.py")

file(MAKE_DIRECTORY "${LAMPOS_GEN_DIR}")

# Scan for the form's effect sources so cmake re-runs the generator if any
# of them change. file(GLOB ...) is normally CMake-poison for SRCS lists but
# here it's only used as a dependency hint to trigger the generator — the
# actual compiled source list comes from the generator's output.
set(LAMPOS_HC_FORM_SRCS "")
if(IS_DIRECTORY "${LAMPOS_HC_FORM}")
    file(GLOB LAMPOS_HC_FORM_SRCS "${LAMPOS_HC_FORM}/*.c")
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND} -E echo "Generating hardcoded effect registry for form '${LAMPOS_FORM}'..."
)
execute_process(
    COMMAND python3 "${LAMPOS_GEN_PY}"
            --form "${LAMPOS_FORM}"
            --src-dir "${LAMPOS_HC_ROOT}"
            --out-dir "${LAMPOS_GEN_DIR}"
    RESULT_VARIABLE LAMPOS_GEN_RC
    OUTPUT_VARIABLE LAMPOS_GEN_OUT
    ERROR_VARIABLE  LAMPOS_GEN_ERR
)
if(NOT LAMPOS_GEN_RC EQUAL 0)
    message(FATAL_ERROR "gen_hardcoded.py failed (${LAMPOS_GEN_RC}):\n${LAMPOS_GEN_ERR}")
endif()
if(LAMPOS_GEN_OUT OR LAMPOS_GEN_ERR)
    message(STATUS "gen_hardcoded.py: ${LAMPOS_GEN_OUT}${LAMPOS_GEN_ERR}")
endif()

# Re-run the generator if any handwritten effect source changes — without
# this, editing fire.c wouldn't refresh the params header / registry.
foreach(src IN LISTS LAMPOS_HC_FORM_SRCS)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${src}")
endforeach()

# Pick up the generated registry + per-effect param .c files.
file(GLOB LAMPOS_GEN_SRCS "${LAMPOS_GEN_DIR}/*.c")

idf_component_register(
    SRCS
        "main.c"
        "config_store.c"
        "wifi.c"
        "captive_portal.c"
        "http_server.c"
        "ws_server.c"
        "led_control.c"
        "light_api.c"
        "error_light.c"
        "ota_update.c"
        "plaiiin_mqtt.c"
        "js_storage.c"
        "js_player.c"
        "js_api.c"
        "mdns_service.c"
        "bt_service.c"
        "buttons.c"
        "factory_reset.c"
        "ai_key_api.c"
        "reset_key_api.c"
        "pairing.c"
        "form_prompt.c"
        "keys.c"
        "wormhole.c"
        "hardcoded_runtime.c"
        ${LAMPOS_HC_FORM_SRCS}
        ${LAMPOS_GEN_SRCS}
    INCLUDE_DIRS
        "."
        "${LAMPOS_GEN_DIR}"
    REQUIRES
        esp_http_server
        json
        mbedtls
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        esp_driver_spi
        driver
        esp_driver_gpio
        esp_system
        esp_partition
        app_update
        spiffs
        mqtt
        plbc
        bt
        button
    EMBED_TXTFILES
        "../portal/style.css"
        "../portal/index.html"
        "../portal/control.html"
        "../portal/network.html"
        "../portal/test.html"
        "../portal/compose.html"
        "../portal/ota.html"
        "../portal/mqtt.html"
        "../portal/js.html"
        "../portal/api.html"
        "../portal/noop.js"
        "../portal/shade-runtime.js"
)

# Bake the form name into the firmware as a compile-time constant so the OTA
# check in light_api.c can verify the incoming binary matches the device.
target_compile_definitions(${COMPONENT_LIB} PRIVATE LAMPOS_FORM_STR=\"${LAMPOS_FORM}\")
