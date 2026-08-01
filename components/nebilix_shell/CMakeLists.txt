idf_component_register(
    SRCS "shell.cpp"
    INCLUDE_DIRS "include"
    REQUIRES console nebilix_kernel nebilix_wifi nebilix_remote nebilix_scripts
)
