set(RLE565_DIR "${CMAKE_CURRENT_LIST_DIR}/../../rle565/src")
set(SIMPLELIB_DIR "${CMAKE_CURRENT_LIST_DIR}/../../SimpleLib")

# Boot-time stress run (main.cpp RUN_STRESS): embeds the test images.
#   idf.py --preset debug -DCB_RUN_STRESS=ON build
set(EMBED "../assets/ui.bin")
if(CB_RUN_STRESS)
    list(APPEND EMBED "../assets/test_keys.bin")
endif()

idf_component_register(
    SRCS
        "main.cpp"
        "screen.cpp"
        "screen_view.cpp"
        "status_bar.cpp"
        "status.cpp"
        "compositor.cpp"
        "page_compositor.cpp"
        "image_store.cpp"
        "key_layout.cpp"
        "model.cpp"
        "protocol.cpp"
        "config_store.cpp"
        "config_flash.cpp"
        "config_blob.cpp"
        "config_apply.cpp"
        "config_boot.cpp"
        "host_link.cpp"
        "usb.cpp"
        "swipe.cpp"
        "touch_router.cpp"
        "touch_gate.cpp"
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
        ${EMBED}
    REQUIRES
        esp_app_format
        esp_driver_gpio
        esp_driver_i2c
        esp_hw_support
        esp_lcd
        esp_partition
        esp_system
        esp_mm
        esp_rom
        esp_timer
        espressif__esp_lcd_touch_gt911
        espressif__esp_tinyusb
)

if(CB_RUN_STRESS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE RUN_STRESS=1)
endif()

# Backlight cycle (main.cpp BACKLIGHT_CYCLE_S): toggles the backlight every N seconds.
#   idf.py --preset debug -DCB_BACKLIGHT_CYCLE_S=5 build   (0 turns it off again)
if(CB_BACKLIGHT_CYCLE_S)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BACKLIGHT_CYCLE_S=${CB_BACKLIGHT_CYCLE_S})
endif()
