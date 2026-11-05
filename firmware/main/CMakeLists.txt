set(RLE565_DIR "${CMAKE_CURRENT_LIST_DIR}/../../rle565/src")
set(SIMPLELIB_DIR "${CMAKE_CURRENT_LIST_DIR}/../../SimpleLib")

# Boot-time stress run (main.cpp RUN_STRESS): embeds the test images.
#   idf.py --preset debug -DCB_RUN_STRESS=ON build
set(EMBED "../assets/ui.bin")
if(CB_RUN_STRESS)
    list(APPEND EMBED "../assets/test_keys.bin")
endif()

# Lua 5.4 (submodule ../../lua), the same source list as the host tests (firmware/lua_sources.txt).
# Default 64-bit numbers. Not libs that need files or the OS (io, os, package, debug, coroutine).
set(LUA_DIR "${CMAKE_CURRENT_LIST_DIR}/../../lua")
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../lua_sources.txt" LUA_SRC_NAMES)
set(LUA_SRCS "")
foreach(f ${LUA_SRC_NAMES})
    list(APPEND LUA_SRCS "${LUA_DIR}/${f}")
endforeach()

idf_component_register(
    SRCS
        ${LUA_SRCS}
        "main.cpp"
        "screen.cpp"
        "screen_view.cpp"
        "status_bar.cpp"
        "status.cpp"
        "fill_gap.cpp"
        "compositor.cpp"
        "page_compositor.cpp"
        "image_store.cpp"
        "key_layout.cpp"
        "model.cpp"
        "protocol.cpp"
        "wifi_credentials.cpp"
        "wifi_state.cpp"
        "wifi.cpp"
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
        "key_gesture.cpp"
        "hid_keyboard.cpp"
        "op_queue.cpp"
        "op_registry.cpp"
        "exec_net.cpp"
        "lua_heap.cpp"
        "script_host.cpp"
        "script_api.cpp"
        "hid_chord.cpp"
        "net_queue.cpp"
        "net_worker.cpp"
        "trigger_dispatch.cpp"
        "activity_switch.cpp"
        "activity_host.cpp"
        "standalone.cpp"
        "standalone_task.cpp"
        "i2c.cpp"
        "ch422g.cpp"
        "waveshare_rgb_lcd_port.cpp"
        "waveshare_touch_port.cpp"
        "${RLE565_DIR}/rle565.c"
    INCLUDE_DIRS
        "."
        "${RLE565_DIR}"
        "${SIMPLELIB_DIR}"
        "${LUA_DIR}"
    EMBED_FILES
        ${EMBED}
    REQUIRES
        esp_app_format
        esp_driver_gpio
        esp_driver_i2c
        esp_event
        esp_http_client
        esp_hw_support
        esp_lcd
        esp_partition
        esp_system
        esp_mm
        esp_netif
        esp_rom
        esp_timer
        esp_wifi
        espressif__esp_lcd_touch_gt911
        espressif__esp_tinyusb
        lwip
        nvs_flash
)

if(CB_RUN_STRESS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE RUN_STRESS=1)
endif()

# Wi-Fi link diagnostics (wifi.cpp WIFI_DIAG): RSSI and a gateway ping every 5 s while connected.
# (One RSSI line and a 4-ping gateway test per connect is always logged.)
#   idf.py --preset debug -DCB_WIFI_DIAG=ON build   (OFF turns it off again)
if(CB_WIFI_DIAG)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WIFI_DIAG=1)
endif()

# Once-a-second stats line (main.cpp STATS_LOG): fps, fill, compositor, images, net.
#   idf.py --preset debug -DCB_STATS_LOG=ON build   (OFF turns it off again)
if(CB_STATS_LOG)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE STATS_LOG=1)
endif()

# Backlight cycle (main.cpp BACKLIGHT_CYCLE_S): toggles the backlight every N seconds.
#   idf.py --preset debug -DCB_BACKLIGHT_CYCLE_S=5 build   (0 turns it off again)
if(CB_BACKLIGHT_CYCLE_S)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BACKLIGHT_CYCLE_S=${CB_BACKLIGHT_CYCLE_S})
endif()
