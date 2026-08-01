idf_component_register(
    SRCS "kernel.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_system esp_timer spi_flash nebilix_wifi
)
