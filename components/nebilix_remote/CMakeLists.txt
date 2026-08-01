idf_component_register(
    SRCS "remote.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_https_server nvs_flash mbedtls nebilix_kernel nebilix_wifi nebilix_scripts
    EMBED_FILES "assets/favicon.ico"
)
