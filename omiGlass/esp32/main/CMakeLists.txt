idf_component_register(
    SRCS
        "app_main.cpp"
        "board_service.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_timer esp_hw_support driver
)
