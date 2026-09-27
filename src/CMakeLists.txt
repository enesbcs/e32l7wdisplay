# Main app component.
#
# esp32-s3 env: the full dashboard application from src/ (headers live in
#               src/include/, added via -Isrc/include in platformio.ini).
# safeboot env: the minimal OTA recovery stub from src/safeboot/ (it
#               provides its own app_main). Keeping it out of components/
#               means it is never auto-discovered by the main env.
if(PIOENV STREQUAL "safeboot")
    FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/safeboot/*.*)
    idf_component_register(SRCS ${app_sources} ${CMAKE_CURRENT_LIST_DIR}/DnsServer.cpp
                           INCLUDE_DIRS . ${CMAKE_CURRENT_LIST_DIR}
                           REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_server
                                    esp_partition app_update esp_app_format bootloader_support
                                    esp_system log esp_rom)
else()
    # sources only (headers never compile as TUs), minus the safeboot stub
    # which provides its own app_main (duplicate symbol if globbed in)
    FILE(GLOB_RECURSE app_sources
         ${CMAKE_SOURCE_DIR}/src/*.cpp
         ${CMAKE_SOURCE_DIR}/src/*.c)
    list(FILTER app_sources EXCLUDE REGEX ".*/src/safeboot/.*")
    idf_component_register(SRCS ${app_sources} REQUIRES bt)
endif()