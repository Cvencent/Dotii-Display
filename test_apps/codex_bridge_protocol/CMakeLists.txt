cmake_minimum_required(VERSION 3.16)

set(IDF_TARGET "esp32s3")
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../managed_components/espressif__cjson"
)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(codex_bridge_protocol)
