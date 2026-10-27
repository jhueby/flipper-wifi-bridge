idf_component_register(SRCS "bridge_main.c"
                       INCLUDE_DIRS ""
                       REQUIRES driver esp_wifi esp_netif nvs_flash lwip
                                app_update esp_partition)
