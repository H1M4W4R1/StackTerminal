cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(StackTerminal VERSION 1)

# M5GFX 0.3.0 queries esp_psram when PSRAM is enabled on ESP-IDF 5.5.
idf_component_get_property(m5gfx_target m5stack__m5gfx COMPONENT_LIB)
target_link_libraries(${m5gfx_target} PRIVATE idf::esp_psram)
