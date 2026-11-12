#include "esp_cpu.h"
#include "esp_timer.h"
#include "gap_share.h"
#include "fill_gap.h"

static fill_gap_client_t *s_clients[FILL_GAP_MAX_CLIENTS];
static int                s_count;
static GapRotor           s_rotor;
static volatile uint32_t  s_end_us;     // low 32 bits of esp_timer_get_time()

void fill_gap_register(fill_gap_client_t *client)
{
    if (s_count < FILL_GAP_MAX_CLIENTS)
        s_clients[s_count++] = client;
}

IRAM_ATTR void fill_gap_done(uint32_t fill_start_cycles)
{
    bool wants[FILL_GAP_MAX_CLIENTS];
    bool any = false;
    for (int i = 0; i < s_count; i++) {
        wants[i] = s_clients[i]->wants && s_clients[i]->task;
        any = any || wants[i];
    }
    if (!any)
        return;

    int c = s_rotor.Next(wants, s_count);
    uint32_t now = (uint32_t)esp_timer_get_time();
    uint32_t fill_us = (esp_cpu_get_cycle_count() - fill_start_cycles) / CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    s_end_us = now;
    s_clients[c]->fill_start_us = now - fill_us;
    xTaskNotifyFromISR(s_clients[c]->task, s_clients[c]->bit, eSetBits, NULL);
}

uint32_t fill_gap_age_us(void)
{
    return (uint32_t)esp_timer_get_time() - s_end_us;
}

bool fill_gap_fresh(uint32_t max_age_us)
{
    return fill_gap_age_us() < max_age_us;
}
