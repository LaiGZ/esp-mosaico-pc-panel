idf_component_register(
    SRCS "main.c" "board_display.c" "pc_panel_ui.c" "pc_link.c" "wifi_manager.c"
        "heap_watch.c" "orientation.c" "mirror.c"
    INCLUDE_DIRS "."
    REQUIRES esp_iris esp-mosaico-bsp esp_mosaico_app_recovery
        esp_mosaico_gsp_bundle esp_mosaico_gsp_iris freertos nvs_flash
        esp_wifi esp_netif esp_event lwip
        espressif2022__bmi270 espressif__i2c_bus
    PRIV_REQUIRES esp_timer esp-gsp esp_display_present esp_lcd)
include("${MOSAICO_UTILS_ROOT}/mosaico-tools/cmake/gsp_bundle.cmake")
mosaico_gsp_add_ui_bundle(${COMPONENT_LIB} "../ui/main.json")
