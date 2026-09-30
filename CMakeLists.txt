cmake_minimum_required(VERSION 3.16)

# Site-specific settings -- WiFi credentials, the update server, the log
# collector -- belong in sdkconfig.defaults.local, which is gitignored. Keeping
# them in a defaults file rather than only in sdkconfig means they are one file
# to back up, and they come back on their own if sdkconfig is ever lost or
# regenerated. The file is optional: without it those options simply fall back
# to their defaults.
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.local")
    list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.local")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ws2812b_clock)
