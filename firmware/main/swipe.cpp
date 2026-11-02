#include <math.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "screen.h"
#include "screen_view.h"
#include "status.h"
#include "swipe.h"

static const char *TAG = "swipe";

// Tuning
#define DRAG_SLOP_PX        10       // finger travel before a press becomes a drag
#define FLICK_SPEED         400.0f   // px/s: faster than this on release = flick to next page
#define VELOCITY_WINDOW_US  80000    // release velocity is measured over the last 80 ms
#define RUBBER_BAND         0.55f    // resistance past the first/last page (higher = looser)

// Glide after release:
//  1 = critically damped spring (iOS-like): leaves at the finger's speed, covers
//      most of the distance quickly, then settles with a short tail. Takes about
//      the same time whatever the distance.
//  0 = cubic ease-out sized to the release velocity (the first version).
#define GLIDE_SPRING        1
#define SPRING_RESPONSE_S   0.30f    // spring "period": lower = snappier
#define SETTLE_MIN_S        0.12f    // ease-out only: shortest glide
#define SETTLE_MAX_S        0.40f    // ease-out only: longest glide
#define SETTLE_DEFAULT_S    0.30f    // ease-out only: glide when release velocity is away from the target

#define PAGE_W ((float)SCREEN_PAGE_W)

typedef struct {
    TOUCH_EVENT_TYPE type;
    int              x;
    int64_t          time_us;
} swipe_event_t;

typedef struct {
    int64_t time_us;
    int     x;
} sample_t;

typedef enum {
    STATE_IDLE,       // resting on s_page
    STATE_PRESSED,    // finger down, not moved far enough to drag
    STATE_DRAGGING,   // page follows the finger
    STATE_SETTLING,   // gliding to s_page after release
} state_t;

static QueueHandle_t s_queue;
static int           s_num_pages;

static state_t s_state = STATE_IDLE;
static float   s_pos;                 // scroll position, px. Page i rests at i * PAGE_W
static volatile int s_page;           // page at rest, or the glide target

// Drag
static int   s_down_x;
static float s_anchor_x;              // finger x when the drag started
static float s_anchor_raw;            // un-rubber-banded position when the drag started

// Recent finger samples for the release velocity
#define NUM_SAMPLES 16
static sample_t s_samples[NUM_SAMPLES];
static int      s_sample_count;
static int      s_sample_next;

// Glide
static float   s_glide_from;
static float   s_glide_to;
#if GLIDE_SPRING
static float   s_glide_b;             // see glide_to()
#else
static float   s_glide_dur;
#endif
static int64_t s_glide_start_us;

// Rubber band: maps distance past the end to a shorter, diminishing distance.
static float band(float x)
{
    return (1.0f - 1.0f / (x * RUBBER_BAND / PAGE_W + 1.0f)) * PAGE_W;
}

static float unband(float y)
{
    if (y >= PAGE_W - 1.0f)
        y = PAGE_W - 1.0f;
    return PAGE_W / RUBBER_BAND * (y / (PAGE_W - y));
}

static float max_pos(void)
{
    return (s_num_pages - 1) * PAGE_W;
}

static float rubber(float raw)
{
    if (raw < 0)
        return -band(-raw);
    if (raw > max_pos())
        return max_pos() + band(raw - max_pos());
    return raw;
}

static float unrubber(float pos)
{
    if (pos < 0)
        return -unband(-pos);
    if (pos > max_pos())
        return max_pos() + unband(pos - max_pos());
    return pos;
}

static void add_sample(int x, int64_t time_us)
{
    s_samples[s_sample_next] = sample_t{ time_us, x };
    s_sample_next = (s_sample_next + 1) % NUM_SAMPLES;
    if (s_sample_count < NUM_SAMPLES)
        s_sample_count++;
}

// Finger velocity in px/s (positive = moving right), from the newest sample
// and the oldest one inside the velocity window.
static float finger_velocity(void)
{
    if (s_sample_count < 2)
        return 0;

    const sample_t *newest = &s_samples[(s_sample_next + NUM_SAMPLES - 1) % NUM_SAMPLES];
    const sample_t *oldest = newest;
    for (int i = 2; i <= s_sample_count; i++) {
        const sample_t *s = &s_samples[(s_sample_next + NUM_SAMPLES - i) % NUM_SAMPLES];
        if (newest->time_us - s->time_us > VELOCITY_WINDOW_US)
            break;
        oldest = s;
    }

    int64_t dt = newest->time_us - oldest->time_us;
    if (dt < 5000)
        return 0;
    return (newest->x - oldest->x) * 1e6f / dt;
}

static int clamp_page(int page)
{
    if (page < 0)
        return 0;
    if (page >= s_num_pages)
        return s_num_pages - 1;
    return page;
}

#define SPRING_OMEGA (2.0f * (float)M_PI / SPRING_RESPONSE_S)

// Glide from the current position to `page`. `velocity` is the scroll
// velocity at release (px/s).
static void glide_to(int page, float velocity, int64_t now)
{
    if (page != s_page)
        ESP_LOGI(TAG, "page %d -> %d", s_page, page);
    s_page = page;

    s_glide_from     = s_pos;
    s_glide_to       = page * PAGE_W;
    s_glide_start_us = now;
    float dist       = s_glide_to - s_glide_from;

    if (fabsf(dist) < 0.5f) {
        s_pos   = s_glide_to;
        s_state = STATE_IDLE;
        return;
    }
    s_state = STATE_SETTLING;

#if GLIDE_SPRING
    // Critically damped spring: offset from target = (a + b*t) * e^(-w*t),
    // with a = starting offset and b chosen so the start speed is `velocity`.
    // It only overshoots if b has the opposite sign to a (a very fast
    // release); clamp b to 0 then, which just caps the start speed.
    float a = -dist;
    float b = velocity + SPRING_OMEGA * a;
    if ((a < 0 && b > 0) || (a > 0 && b < 0))
        b = 0;
    s_glide_b = b;
#else
    // Cubic ease-out: starts at speed 3*d/T and slows to a stop without
    // overshooting. T is chosen so the glide starts at the release velocity,
    // within limits.
    float dur = SETTLE_DEFAULT_S;
    if (velocity * dist > 0)
        dur = 3.0f * fabsf(dist) / fabsf(velocity);
    if (dur < SETTLE_MIN_S)
        dur = SETTLE_MIN_S;
    if (dur > SETTLE_MAX_S)
        dur = SETTLE_MAX_S;
    s_glide_dur = dur;
#endif
}

// Glide position at `now`. Returns false once the glide has finished.
static bool glide_step(int64_t now)
{
    float t = (now - s_glide_start_us) / 1e6f;

#if GLIDE_SPRING
    float a      = s_glide_from - s_glide_to;
    float offset = (a + s_glide_b * t) * expf(-SPRING_OMEGA * t);
    if (fabsf(offset) < 0.5f && t > 0.05f)
        return false;
    s_pos = s_glide_to + offset;
#else
    float s = t / s_glide_dur;
    if (s >= 1.0f)
        return false;
    float inv = 1.0f - s;
    s_pos = s_glide_from + (s_glide_to - s_glide_from) * (1.0f - inv * inv * inv);
#endif
    return true;
}

static void on_release(int64_t now)
{
    float velocity = -finger_velocity();      // finger right = scroll back
    int   target;

    if (fabsf(velocity) > FLICK_SPEED) {
        // Flick: go to the next page boundary in the flick direction, but
        // never more than one page from where the drag started.
        float p = s_pos / PAGE_W;
        target = velocity > 0 ? (int)floorf(p) + 1 : (int)ceilf(p) - 1;
        if (target > s_page + 1)
            target = s_page + 1;
        if (target < s_page - 1)
            target = s_page - 1;
    } else {
        // Slow release: whichever page is mostly in view (spring back if
        // the drag didn't get past halfway).
        target = (int)lroundf(s_pos / PAGE_W);
    }

    glide_to(clamp_page(target), velocity, now);
}

static void handle_event(const swipe_event_t *ev)
{
    switch (ev->type) {
    case TOUCH_DOWN:
        s_sample_count = 0;
        add_sample(ev->x, ev->time_us);
        if (s_state == STATE_SETTLING || s_state == STATE_DRAGGING) {
            // Catch the page mid-glide and drag from where it is. DRAGGING
            // here means the last TOUCH_UP was lost; treat it the same way.
            s_page       = clamp_page((int)lroundf(s_pos / PAGE_W));
            s_anchor_x   = ev->x;
            s_anchor_raw = unrubber(s_pos);
            s_state      = STATE_DRAGGING;
        } else {
            s_down_x = ev->x;
            s_state  = STATE_PRESSED;
        }
        break;

    case TOUCH_MOVE:
        add_sample(ev->x, ev->time_us);
        if (s_state == STATE_PRESSED && abs(ev->x - s_down_x) > DRAG_SLOP_PX) {
            // Start dragging from here, so the page doesn't jump by the slop
            s_anchor_x   = ev->x;
            s_anchor_raw = s_pos;
            s_state      = STATE_DRAGGING;
        }
        if (s_state == STATE_DRAGGING)
            s_pos = rubber(s_anchor_raw - (ev->x - s_anchor_x));
        break;

    case TOUCH_UP:
        add_sample(ev->x, ev->time_us);
        if (s_state == STATE_DRAGGING)
            on_release(ev->time_us);
        else if (s_state == STATE_PRESSED)
            s_state = STATE_IDLE;     // a tap; key presses come later
        break;
    }
}

static void swipe_task(void *arg)
{
    while (1) {
        // Woken at the start of every frame. What's computed here shows on
        // the next frame.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int64_t now = esp_timer_get_time();

        swipe_event_t ev;
        while (xQueueReceive(s_queue, &ev, 0))
            handle_event(&ev);

        if (s_state == STATE_SETTLING && !glide_step(now)) {
            s_pos   = s_glide_to;
            s_state = STATE_IDLE;
        }

        int scroll = (int)lroundf(s_pos);
        screen_set_scroll(scroll);
        status_set_selected_pill(screen_page_most_in_view(scroll, s_num_pages));
    }
}

esp_err_t swipe_init(void)
{
    s_num_pages = screen_page_count();
    s_queue = xQueueCreate(32, sizeof(swipe_event_t));
    if (!s_queue)
        return ESP_ERR_NO_MEM;

    TaskHandle_t task;
    if (xTaskCreatePinnedToCore(swipe_task, "swipe", 4096, NULL, 10, &task, 1) != pdPASS)
        return ESP_FAIL;
    screen_set_frame_task(task);
    return ESP_OK;
}

void swipe_touch(uint8_t id, TOUCH_EVENT_TYPE type, uint16_t x, uint16_t y)
{
    if (id != 0)        // paging follows the first finger only
        return;
    swipe_event_t ev = { .type = type, .x = x, .time_us = esp_timer_get_time() };
    if (xQueueSend(s_queue, &ev, 0) != pdTRUE) {
        static int dropped;
        ESP_LOGW(TAG, "touch queue full, dropped %d events", ++dropped);
    }
}

int swipe_current_page(void)
{
    return s_page;
}
