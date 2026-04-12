#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "esp_freertos_hooks.h"
#include "esp_timer.h"
#include "cpu_monitor.h"

/*
 * CPU load estimation via idle hook.
 *
 * The idle hook increments a counter every time the idle task runs.
 * A 1-second periodic timer samples the delta and tracks the maximum
 * observed idle count (calibration baseline = 0 % load).
 *
 * cpu_load = 100 - (delta / max_delta * 100)
 *
 * The baseline self-calibrates upward whenever a quieter period is seen,
 * so it converges toward the true maximum over time.
 */

static atomic_uint s_idle_ticks  = 0;
static uint32_t    s_prev_ticks  = 0;
static uint32_t    s_max_ticks   = 0;   /* calibration baseline */
static uint8_t     s_cpu_load    = 0;   /* 0–100 % */

static bool idle_hook_cb(void)
{
    s_idle_ticks++;
    return false;   /* do not block further hooks */
}

static void sample_cb(void *arg)
{
    uint32_t curr  = (uint32_t)atomic_load(&s_idle_ticks);
    uint32_t delta = curr - s_prev_ticks;
    s_prev_ticks   = curr;

    /* Update calibration baseline */
    if (delta > s_max_ticks) {
        s_max_ticks = delta;
    }

    if (s_max_ticks > 0) {
        uint32_t load = 100u - (delta * 100u / s_max_ticks);
        s_cpu_load = (uint8_t)(load > 100u ? 100u : load);
    }
}

void cpu_monitor_init(void)
{
    esp_register_freertos_idle_hook(idle_hook_cb);

    const esp_timer_create_args_t args = {
        .callback = sample_cb,
        .name     = "cpu_mon",
    };
    esp_timer_handle_t timer;
    esp_timer_create(&args, &timer);
    esp_timer_start_periodic(timer, 1000ULL * 1000ULL); /* 1 s */
}

uint8_t cpu_monitor_get_load(void)
{
    return s_cpu_load;
}
