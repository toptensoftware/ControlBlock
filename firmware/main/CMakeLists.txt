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
        "status_icons.cpp"
        "log_ring.cpp"
        "log_capture.cpp"
        "diag_status.cpp"
        "diag_collect.cpp"
        "diag_http.cpp"
        "device_name.cpp"
        "clock_text.cpp"
        "status.cpp"
        "fill_gap.cpp"
        "compositor.cpp"
        "page_compositor.cpp"
        "key_geometry.cpp"
        "opaque_row.cpp"
        "image_store.cpp"
        "link_stats.cpp"
        "key_layout.cpp"
        "model.cpp"
        "protocol.cpp"
        "nvs_blob.cpp"
        "clock_config.cpp"
        "clock.cpp"
        "wifi_credentials.cpp"
        "wifi_state.cpp"
        "wifi.cpp"
        "layout_store.cpp"
        "layout_flash.cpp"
        "layout_blob.cpp"
        "layout_apply.cpp"
        "layout_boot.cpp"
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
        esp_http_server
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
        espressif__mdns
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

# Diagnostic: everything but the display (screen.cpp NO_DISPLAY): no panel, no bounce fill ISR,
# backlight left off. For telling display load apart from other causes (wip/wip-wifi-latency.md).
#   idf.py --preset debug -DCB_NO_DISPLAY=ON build   (OFF turns it off again)
if(CB_NO_DISPLAY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NO_DISPLAY=1)
endif()

# Diagnostic: the panel runs but the bounce fill does nothing (screen.cpp IDLE_FILL): the
# panel's signals without the fill's CPU and PSRAM load (wip/wip-wifi-latency.md).
#   idf.py --preset debug -DCB_IDLE_FILL=ON build   (OFF turns it off again)
if(CB_IDLE_FILL)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE IDLE_FILL=1)
endif()

# Pixel clock experiment (waveshare_rgb_lcd_port.h PIXEL_CLOCK_MHZ): whole MHz, default 20 (~49 fps).
# The fill budget follows it (~615 us at 20 MHz). idf.py has no -U: an empty value removes it.
#   idf.py --preset release -DCB_PIXEL_CLOCK_MHZ=20 build   (-DCB_PIXEL_CLOCK_MHZ= goes back to 25)
if(CB_PIXEL_CLOCK_MHZ)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PIXEL_CLOCK_MHZ=${CB_PIXEL_CLOCK_MHZ})
endif()

# Once-a-second stats line (main.cpp STATS_LOG): fps, fill, compositor, images, usb rx, net.
#   idf.py --preset debug -DCB_STATS_LOG=ON build   (OFF turns it off again)
if(CB_STATS_LOG)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE STATS_LOG=1)
endif()

# Backlight switching (waveshare_rgb_lcd_port.cpp BACKLIGHT_SWITCH): off by default, the light
# stays on (a switch-on corrupts PSRAM, CLAUDE.md Open Items). For supply experiments.
#   idf.py --preset debug -DCB_BACKLIGHT_SWITCH=ON build   (OFF turns it off again)
# Backlight cycle (main.cpp BACKLIGHT_CYCLE_S): toggles the backlight every N seconds; turns
# switching on too.
#   idf.py --preset debug -DCB_BACKLIGHT_CYCLE_S=5 build   (0 turns it off again)
if(CB_BACKLIGHT_SWITCH OR CB_BACKLIGHT_CYCLE_S)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BACKLIGHT_SWITCH=1)
endif()
if(CB_BACKLIGHT_CYCLE_S)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BACKLIGHT_CYCLE_S=${CB_BACKLIGHT_CYCLE_S})
endif()
