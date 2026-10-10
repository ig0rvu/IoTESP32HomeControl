idf_component_register( SRCS "tcp_client.c" "IoT_generic_main.c" "wifi.c" "proto/controlhome.pb.c" "gpio.c" "rfid.c" "ap_mode.c" "ota.c"
                        PRIV_REQUIRES esp_http_server  spi_flash esp_wifi esp_event nvs_flash esp_netif nanopb lwip esp_timer esp_driver_gpio esp_driver_uart app_update esp_https_ota esp_http_client
                        INCLUDE_DIRS "proto"
                        EMBED_TXTFILES "html/ap_config.html" "html/pins.json")
