idf_component_register(SRCS "hello_world_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_psram
                       PRIV_REQUIRES spi_flash
                       )