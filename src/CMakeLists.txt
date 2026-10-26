idf_component_register(
    SRCS
        ble_keyboard.cpp
        file_manager.cpp
        git_fs.cpp
        git_merge.c
        git_net.c
        git_odb.c
        git_pack.c
        git_sync.cpp
        git_util.c
        input_handler.cpp
        keyboard_layout.cpp
        main.cpp
        text_editor.cpp
        ui_renderer.cpp
        wifi_sync.cpp
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_http_client esp-tls mbedtls esp_rom esp_netif lwip
)
