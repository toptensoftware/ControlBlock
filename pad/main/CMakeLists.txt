# The pure, host-tested modules live in ../common (pad/test runs them on the PC); main.cpp is glue.
# PacketEncoder is a submodule of C files (no name clash: the S3's crc32.h became crc32_table.h).
set(COMMON_DIR "${CMAKE_CURRENT_LIST_DIR}/../common")
set(PACKENC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../PacketEncoder")

idf_component_register(
    SRCS
        "main.cpp"
        "${COMMON_DIR}/pad_protocol.cpp"
        "${COMMON_DIR}/glitch_filter.cpp"
        "${COMMON_DIR}/quad_decoder.cpp"
        "${COMMON_DIR}/encoder_channel.cpp"
        "${COMMON_DIR}/pad_scanner.cpp"
        "${PACKENC_DIR}/packenc.c"
        "${PACKENC_DIR}/crc32.c"
    INCLUDE_DIRS
        "."
        "${COMMON_DIR}"
        "${PACKENC_DIR}"
    REQUIRES
        esp_driver_gpio
        esp_driver_uart
        esp_timer
        freertos
)
