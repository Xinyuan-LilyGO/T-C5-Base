idf_component_register(SRCS "rs485_dual_speed_test.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_uart esp_timer)
