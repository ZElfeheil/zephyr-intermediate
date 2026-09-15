#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(demo, LOG_LEVEL_DBG);

#define STACK_SIZE 1024
#define PRIO_WORKER 5
#define ITERATIONS 1000U
#define WORKER_COUNT 2U

/* Shared counter protected by counter_mutex. */
static uint32_t shared_counter;
K_MUTEX_DEFINE(counter_mutex);
/* Define semaphore*/
static K_SEM_DEFINE(workers_done, 0, WORKER_COUNT);
/*Define worker thread stack size*/
K_THREAD_STACK_DEFINE(worker_a_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(worker_b_stack, STACK_SIZE);

static struct k_thread worker_a;
static struct k_thread worker_b;

void increment_counter(void *p1, void *p2, void *p3)
{
    const char *name = p1;

    for (uint32_t i = 0U; i < ITERATIONS; i++) {
        k_mutex_lock(&counter_mutex, K_FOREVER);
        shared_counter++;
        k_mutex_unlock(&counter_mutex);
    }

    LOG_INF("%s finished", name);
    k_sem_give(&workers_done);
}

int main(void)
{
    const uint32_t expected = WORKER_COUNT * ITERATIONS;

    LOG_INF("=== Mutex-protected counter demo ===");
    LOG_INF("Two priority-%d threads each increment %u times", PRIO_WORKER,
            ITERATIONS);

    k_thread_create(&worker_a, worker_a_stack, STACK_SIZE, increment_counter,
                    "worker A", NULL, NULL, PRIO_WORKER, 0, K_NO_WAIT);
    k_thread_create(&worker_b, worker_b_stack, STACK_SIZE, increment_counter,
                    "worker B", NULL, NULL, PRIO_WORKER, 0, K_NO_WAIT);

    k_sem_take(&workers_done, K_FOREVER);
    k_sem_take(&workers_done, K_FOREVER);

    LOG_INF("Expected counter: %u", expected);
    LOG_INF("Actual counter:   %u", shared_counter);
    LOG_INF("Lost updates:     %u", expected - shared_counter);

    return 0;
}
