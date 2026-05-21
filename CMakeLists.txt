cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    $ENV{IDF_PATH}/examples/common_components/protocol_examples_common
    "${CMAKE_CURRENT_LIST_DIR}/components"
)

# Pull firmware.version from version.properties and export as PROJECT_VER so
# ESP-IDF embeds it into esp_app_desc_t (what /api/ota/info exposes as
# "version"). Without this, ESP-IDF falls back to `git describe --dirty`,
# which always reports "<sha>-dirty" because the build itself touches the
# tree (sdkconfig regen, version.properties bump, etc.).
set(_version_file "${CMAKE_CURRENT_LIST_DIR}/version.properties")
if(EXISTS "${_version_file}")
    file(STRINGS "${_version_file}" _version_lines)
    foreach(_line IN LISTS _version_lines)
        if(_line MATCHES "^[ \t]*firmware\\.version[ \t]*=[ \t]*(.+)$")
            string(STRIP "${CMAKE_MATCH_1}" PROJECT_VER)
        endif()
    endforeach()
endif()
if(NOT DEFINED PROJECT_VER OR PROJECT_VER STREQUAL "")
    set(PROJECT_VER "0.0.0")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Phase 35 — per-form firmware. Bake the form name into the project name so
# esp_app_desc.project_name reads "plaiiinlight_os_<form>". /api/ota uses
# that to refuse a binary built for a different form than the device.
#
# We read LAMPOS_FORM from the environment, not a -D cache var: ESP-IDF
# invokes `component_get_requirements` in a sub-cmake process that doesn't
# inherit -D, so cache values get dropped on the way down. Environment
# variables survive the sub-process boundary cleanly. build.sh exports the
# variable before calling idf.py.
if(DEFINED ENV{LAMPOS_FORM} AND NOT "$ENV{LAMPOS_FORM}" STREQUAL "")
    set(LAMPOS_FORM "$ENV{LAMPOS_FORM}")
endif()
if(NOT DEFINED LAMPOS_FORM OR LAMPOS_FORM STREQUAL "")
    message(FATAL_ERROR
        "LAMPOS_FORM is not set. Pass it via scripts/build.sh --form <name> "
        "(which `export`s LAMPOS_FORM before invoking idf.py).")
endif()
project(plaiiinlight_os_${LAMPOS_FORM})
