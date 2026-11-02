set(RLE565_DIR "${CMAKE_CURRENT_LIST_DIR}/../../rle565/src")
set(SIMPLELIB_DIR "${CMAKE_CURRENT_LIST_DIR}/../../SimpleLib")

idf_component_register(
    SRCS
        "main.cpp"
        "screen.cpp"
        "screen_view.cpp"
        "status_bar.cpp"
        "status.cpp"
        "compositor.cpp"
        "page_compositor.cpp"
        "swipe.cpp"
        "i2c.cpp"
        "ch422g.cpp"
        "waveshare_rgb_lcd_port.cpp"
        "waveshare_touch_port.cpp"
        "${RLE565_DIR}/rle565.c"
    INCLUDE_DIRS
        "."
        "${RLE565_DIR}"
        "${SIMPLELIB_DIR}"
    EMBED_FILES
        "../assets/test_keys.bin"
        "../assets/ui.bin"
    REQUIRES
        esp_driver_gpio
        esp_driver_i2c
        esp_hw_support
        esp_lcd
        esp_mm
        esp_rom
        esp_timer
        espressif__esp_lcd_touch_gt911
)
