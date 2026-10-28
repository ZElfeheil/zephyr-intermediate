#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/tracing/tracing.h>

LOG_MODULE_REGISTER(homework, LOG_LEVEL_INF);

#define STACK_SIZE            2048
#define CONTROL_PRIORITY         7
#define MAINTENANCE_PRIORITY     4
#define EVENT_PERIOD_MS        250
#define MAINTENANCE_LOAD_US  45000
#define CONTROL_RESPONSE_GUARANTEE_MS 10U

struct control_event {
    uint32_t seq;
    uint32_t ready_ms;
};

K_MSGQ_DEFINE(control_queue, sizeof(struct control_event), 4, 4);
K_SEM_DEFINE(maintenance_start, 0, 1);

/* ================================================================== */
/*  Timer expiry: creates one control event                           */
/* ================================================================== */

static void event_timer_expiry(struct k_timer *timer)
{
    ARG_UNUSED(timer);

    static uint32_t seq;
    struct control_event event = {
        .seq = seq++,
        .ready_ms = k_uptime_get_32(),
    };

    /* Timer expiry runs in interrupt context, so never wait here. */
    int ret = k_msgq_put(&control_queue, &event, K_NO_WAIT);

    if (ret != 0) {
        return;
    }

    /* Both threads become ready when the timer interrupt returns. */
    k_sem_give(&maintenance_start);

    /* TODO: Add an application trace event for this sequence. */
}

K_TIMER_DEFINE(event_timer, event_timer_expiry, NULL);

/* ================================================================== */
/*  Control thread                                                   */
/* ================================================================== */

static void control_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
    uint32_t deadline_misses = 0;

    while (true) {
        struct control_event event;
        int ret = k_msgq_get(&control_queue, &event, K_FOREVER);

        if (ret != 0) {
            LOG_ERR("[CONTROL] receive failed: %d", ret);
            continue;
        }

        LOG_INF("[CONTROL] processed seq=%u", event.seq);

        BUILD_ASSERT(CONTROL_RESPONSE_GUARANTEE_MS == 10U,
                     "control response-time guarantee must be 10 ms");
        /* TODO: Measure latency and count every deadline miss. */
        /* TODO: Rate-limit repeated warning messages. */
        uint32_t latency_ms = k_uptime_get_32()- event.ready_ms;
        if (latency_ms > CONTROL_RESPONSE_GUARANTEE_MS) {
            deadline_misses++;
            LOG_WRN_RATELIMIT(
                "deadline_miss count=%u seq=%u latency_ms=%u",
                deadline_misses, event.seq, latency_ms);
        } else {
                LOG_INF("[Control] event_done seq=%u latency=%ums",
                    event.seq, latency_ms);
        }
        sys_trace_named_event("control_event_complete", event.seq, latency_ms);
    }
}

/* ================================================================== */
/*  Background maintenance thread                                    */
/* ================================================================== */

static void maintenance_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    while (true) {
        k_sem_take(&maintenance_start, K_FOREVER);

        /* This work is important, but it has no short deadline. */
        k_busy_wait(MAINTENANCE_LOAD_US);
    }
}

K_THREAD_DEFINE(control, STACK_SIZE, control_fn,
                NULL, NULL, NULL, CONTROL_PRIORITY, 0, 0);

K_THREAD_DEFINE(maintenance, STACK_SIZE, maintenance_fn,
                NULL, NULL, NULL, MAINTENANCE_PRIORITY, 0, 0);

/* Kernel-shell inspection on the board:
 * - maintenance: priority 4, pending, stack usage 12%
 * - control: priority 7, pending, stack usage 17%
 * - shell_uart: priority 14, queued, stack usage 55%
 * - logging: priority 14, pending, stack usage 64%
 * - idle: priority 15, stack usage 37%
 *
 * Stack headroom is healthy. The observed 45 ms deadline misses are caused
 * by maintenance_fn's busy wait delaying the lower-priority control thread,
 * which has a 10 ms response-time guarantee.
 */

int main(void)
{
    LOG_INF("=== L6 Homework: Runtime Investigation ===");
    LOG_INF("Control work must start within %u ms",
            CONTROL_RESPONSE_GUARANTEE_MS);
    LOG_INF("Inspect, measure, trace, explain, and correct the delay");

    k_timer_start(&event_timer, K_MSEC(500), K_MSEC(EVENT_PERIOD_MS));

    return 0;
}
