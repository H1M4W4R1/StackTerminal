idf_component_register(SRCS "main.cpp" "terminal.cpp" "terminal_view.cpp" "usb_keyboard.cpp"
                            "${CMAKE_CURRENT_BINARY_DIR}/terminal_fonts.S"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_uart esp_timer nvs_flash usb)

configure_file("fonts/fonts.S.in" "${CMAKE_CURRENT_BINARY_DIR}/terminal_fonts.S" @ONLY)
file(GLOB terminal_fonts CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/fonts/*.bin")
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/terminal_fonts.S"
                            PROPERTIES OBJECT_DEPENDS "${terminal_fonts}")
