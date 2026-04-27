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
project(plaiiinlight_os)
