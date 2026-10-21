idf_component_register(
    SRCS
        "app_main.cpp"
        "board_service.cpp"
        "mic_service.cpp"
        "opus_service.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        esp_timer
        esp_hw_support
        esp_driver_gpio
        esp_driver_i2s
        heap
)
