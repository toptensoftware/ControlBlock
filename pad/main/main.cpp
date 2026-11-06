// Macro pad firmware (ESP32-C3): glue between the hardware and PadScanner (pad/common, all
// logic is there and host tested). This file only reads pins, stamps encoder edges, calls
// the scanner in time order and writes its bytes to UART1.
//
// Hardware only: nothing in this file can run in the host tests. See pad/README.md.
//
// Threads:
//   GPIO ISR   (every encoder pin, both edges): reads both pins of that encoder, pushes
//              {esp_timer time, encoder, a, b} into a single-producer single-consumer ring.
//   pad task   (high priority): every 1 ms, drains the ring in order into
//              PadScanner::SetEncoderPins(stamp, ...), reads the keys, calls Poll, flushes
//              the packet bytes to the UART.
//   stats task (low priority): logs one line a second on UART0. Logging is slow (7 ms for
//              80 characters at 115200) so it must never run in the pad task.
#include <stdint.h>
#include <stddef.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pad_pins.h"
#include "pad_scanner.h"

namespace
{
    const char *TAG = "pad";

    constexpr uart_port_t LINK_UART = UART_NUM_1;

    // ---- Encoder edge ring (ISR -> task) ----------------------------------------------

    struct EdgeRecord
    {
        uint64_t time_us;
        uint8_t  encoder;
        uint8_t  a;
        uint8_t  b;
    };

    constexpr uint32_t RING_SIZE = 256;     // power of two; ~1 ms drain at a few edges per ms is plenty
    static_assert((RING_SIZE & (RING_SIZE - 1)) == 0, "RING_SIZE must be a power of two");

    EdgeRecord s_ring[RING_SIZE];
    volatile uint32_t s_head = 0;           // written by the ISR only
    volatile uint32_t s_tail = 0;           // written by the pad task only
    volatile uint32_t s_overruns = 0;       // edges dropped because the ring was full (ISR)

    void IRAM_ATTR EncoderIsr(void *arg)
    {
        int e = (int)(intptr_t)arg;
        uint32_t head = s_head;
        if (head - __atomic_load_n(&s_tail, __ATOMIC_ACQUIRE) >= RING_SIZE) {
            s_overruns = s_overruns + 1;
            return;
        }
        EdgeRecord &r = s_ring[head & (RING_SIZE - 1)];
        r.time_us = (uint64_t)esp_timer_get_time();
        r.encoder = (uint8_t)e;
        r.a = (uint8_t)gpio_get_level((gpio_num_t)PAD_ENCODER_PINS[e][0]);
        r.b = (uint8_t)gpio_get_level((gpio_num_t)PAD_ENCODER_PINS[e][1]);
        __atomic_store_n(&s_head, head + 1, __ATOMIC_RELEASE);
    }

    // ---- UART link (PadScanner's sink is a bare function pointer) ---------------------

    uint8_t s_tx[256];
    size_t s_tx_len = 0;
    uint32_t s_tx_dropped = 0;

    void TxSink(uint8_t byte)
    {
        if (s_tx_len < sizeof(s_tx))
            s_tx[s_tx_len++] = byte;
        else
            s_tx_dropped++;
    }

    void FlushTx()
    {
        if (s_tx_len == 0)
            return;
        uart_write_bytes(LINK_UART, s_tx, s_tx_len);
        s_tx_len = 0;
    }

    // ---- Stats (written by the pad task, read by the stats task) -----------------------

    volatile uint32_t s_max_latency_us = 0;     // oldest edge's wait in the ring, per second
    volatile uint32_t s_packets = 0;            // scanner's seq (HELLOs and STATEs)

    // ---- Pins -------------------------------------------------------------------------

    PadScanner s_scanner;

    void ConfigInput(int pin, gpio_int_type_t intr)
    {
        gpio_config_t io = {};
        io.pin_bit_mask = 1ULL << pin;
        io.mode = GPIO_MODE_INPUT;
        io.pull_up_en = GPIO_PULLUP_ENABLE;
        io.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io.intr_type = intr;
        ESP_ERROR_CHECK(gpio_config(&io));
    }

    // Bit n set = key n+1 pin HIGH (released)
    uint32_t ReadKeyLevels()
    {
        uint32_t mask = 0;
        for (int k = 0; k < PAD_KEY_COUNT; k++)
            if (gpio_get_level((gpio_num_t)PAD_KEY_PINS[k]))
                mask |= 1u << k;
        return mask;
    }

    // Drains the ring into the scanner, oldest first (the filters need the calls in time
    // order, each with the time of its edge). 'now_us' only measures the wait.
    void DrainEdges(uint64_t now_us)
    {
        uint32_t tail = s_tail;
        uint32_t head = __atomic_load_n(&s_head, __ATOMIC_ACQUIRE);
        if (tail != head) {
            uint64_t oldest = s_ring[tail & (RING_SIZE - 1)].time_us;
            uint32_t latency = (uint32_t)(now_us - oldest);
            if (latency > s_max_latency_us)
                s_max_latency_us = latency;
        }
        while (tail != head) {
            const EdgeRecord &r = s_ring[tail & (RING_SIZE - 1)];
            s_scanner.SetEncoderPins(r.time_us, r.encoder, r.a != 0, r.b != 0);
            FlushTx();
            tail++;
        }
        __atomic_store_n(&s_tail, tail, __ATOMIC_RELEASE);
    }

    void PadTask(void *)
    {
        TickType_t last = xTaskGetTickCount();
        for (;;) {
            vTaskDelayUntil(&last, 1);

            // Edges first, oldest first. The ISR stamps and publishes in one go on this single
            // core, so a record not yet in the ring is later than any clock read after the
            // drain: the times given to the scanner never go backwards.
            DrainEdges((uint64_t)esp_timer_get_time());

            // Keys are sampled now (1 ms resolution; their filter is 5 ms)
            uint64_t now = (uint64_t)esp_timer_get_time();
            s_scanner.SetKeyLevels(now, ReadKeyLevels());
            s_scanner.Poll(now);
            FlushTx();
            s_packets = s_scanner.NextSeq();
        }
    }

    void StatsTask(void *)
    {
        uint32_t last_packets = 0;
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            uint32_t packets = s_packets;
            uint32_t latency = s_max_latency_us;
            s_max_latency_us = 0;
            ESP_LOGI(TAG, "pad: sent %u (+%u) overruns %u txdrop %u maxdrain %u us keys %08x det %u",
                     (unsigned)packets, (unsigned)(packets - last_packets),
                     (unsigned)s_overruns, (unsigned)s_tx_dropped, (unsigned)latency,
                     (unsigned)s_scanner.PressedKeys(),
                     (unsigned)(PAD_ENCODER_COUNT > 0 ? s_scanner.Detents(0) : 0));
            last_packets = packets;
        }
    }
}

extern "C" void app_main(void)
{
    // Link: UART1 TX only
    uart_config_t uc = {};
    uc.baud_rate = PAD_LINK_BAUD;
    uc.data_bits = UART_DATA_8_BITS;
    uc.parity = UART_PARITY_DISABLE;
    uc.stop_bits = UART_STOP_BITS_1;
    uc.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uc.source_clk = UART_SCLK_DEFAULT;
    ESP_ERROR_CHECK(uart_param_config(LINK_UART, &uc));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART, PAD_LINK_TX_GPIO, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    // The driver wants an RX buffer bigger than the 128-byte hardware FIFO even if unused
    ESP_ERROR_CHECK(uart_driver_install(LINK_UART, 256, 512, 0, nullptr, 0));

    // Inputs: pull-ups, active low. Keys are polled; encoder pins interrupt on both edges.
    for (int k = 0; k < PAD_KEY_COUNT; k++)
        ConfigInput(PAD_KEY_PINS[k], GPIO_INTR_DISABLE);
    for (int e = 0; e < PAD_ENCODER_COUNT; e++) {
        ConfigInput(PAD_ENCODER_PINS[e][0], GPIO_INTR_ANYEDGE);
        ConfigInput(PAD_ENCODER_PINS[e][1], GPIO_INTR_ANYEDGE);
    }

    // Start the scanner (this writes the first HELLO) before the ISRs are armed
    uint8_t enc_pins[PAD_MAX_ENCODERS] = {};
    for (int e = 0; e < PAD_ENCODER_COUNT; e++)
        enc_pins[e] = (uint8_t)((gpio_get_level((gpio_num_t)PAD_ENCODER_PINS[e][0]) << 1) |
                                gpio_get_level((gpio_num_t)PAD_ENCODER_PINS[e][1]));
    PadScannerConfig cfg = {};
    cfg.key_count = PAD_KEY_COUNT;
    cfg.encoder_count = PAD_ENCODER_COUNT;
    s_scanner.Init(cfg, TxSink, (uint64_t)esp_timer_get_time(), ReadKeyLevels(), enc_pins);
    FlushTx();

    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));
    for (int e = 0; e < PAD_ENCODER_COUNT; e++) {
        ESP_ERROR_CHECK(gpio_isr_handler_add((gpio_num_t)PAD_ENCODER_PINS[e][0], EncoderIsr, (void *)(intptr_t)e));
        ESP_ERROR_CHECK(gpio_isr_handler_add((gpio_num_t)PAD_ENCODER_PINS[e][1], EncoderIsr, (void *)(intptr_t)e));
    }

    ESP_LOGI(TAG, "pad: %d keys, %d encoders, link TX GPIO %d at %d baud",
             PAD_KEY_COUNT, PAD_ENCODER_COUNT, PAD_LINK_TX_GPIO, PAD_LINK_BAUD);

    xTaskCreate(PadTask, "pad", 4096, nullptr, configMAX_PRIORITIES - 2, nullptr);
    xTaskCreate(StatsTask, "stats", 3072, nullptr, 1, nullptr);
}
