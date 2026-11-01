idf_component_register(SRCS "main.c" "hw.c" "core.c" "net.c" INCLUDE_DIRS "." EMBED_TXTFILES "index.html"
  PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_timer esp_http_server esp_http_client esp-tls
                mbedtls json driver esp_adc mqtt lwip u8g2 u8g2-hal-esp-idf)
