#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(demo, LOG_LEVEL_DBG);

#define STACK_SIZE 1024

#define PRIO_LOW  7
#define PRIO_MED  5
#define PRIO_HIGH 3
#define PRIO_COOP -1

#define BUSY_WORK_CYCLES 100000U

void thread_low_fn(void *p1, void *p2, void *p3)
{
    int step = 0;
    while (1) {
        LOG_INF("T_LOW running: step=%d, uptime=%u ms",
        step++, k_uptime_get_32());
        k_msleep(300);
    }
}

void thread_med_fn(void *p1, void *p2, void *p3)
{
    int step = 0;
    while (1) {
        LOG_INF("T_MED running: step=%d, uptime=%u ms",
        step++, k_uptime_get_32());
        k_msleep(200);
    }
}

void thread_high_fn(void *p1, void *p2, void *p3)
{
    int step = 0;
    while (1) {
        LOG_INF("T_HIGH running: step=%d, uptime=%u ms",
        step++, k_uptime_get_32());
        k_msleep(100);
    }
}

void thread_coop_fn(void *p1, void *p2, void *p3)
{
    uint32_t batch = 0U;

    while (1) {
        for (int iteration = 0; iteration < 5; iteration++) {
            volatile uint32_t work = 0U;

            for (uint32_t cycle = 0U; cycle < BUSY_WORK_CYCLES; cycle++) {
                work += cycle;
            }
        }

        LOG_INF("T_COOP completed batch %u; yielding", batch++);
        k_msleep(1);
        k_yield();
    }
}

K_THREAD_DEFINE(thread_a, STACK_SIZE, thread_low_fn,
                NULL, NULL, NULL, PRIO_LOW, 0, 0);
K_THREAD_DEFINE(thread_b, STACK_SIZE, thread_med_fn,
                NULL, NULL, NULL, PRIO_MED, 0, 0);
K_THREAD_DEFINE(thread_c, STACK_SIZE, thread_high_fn,
                NULL, NULL, NULL, PRIO_HIGH, 0, 0);
K_THREAD_DEFINE(thread_coop, STACK_SIZE, thread_coop_fn,
                NULL, NULL, NULL, PRIO_COOP, 0, 0);

int main(void)
{
    return 0;
}
