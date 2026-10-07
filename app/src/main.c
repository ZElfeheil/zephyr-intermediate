#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <errno.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(demo, LOG_LEVEL_DBG);

/* Demo settings: keep numeric values here. */
#define STACK_SIZE                  2048
#define SENSOR_COUNT                18
#define SENSOR_PERIOD_MS            100
#define SENSOR_PUBLISH_TIMEOUT_MS   100
#define SENSOR_TEMP_BASE_MC         24000
#define SENSOR_TEMP_STEP_MC         350
#define SENSOR_WDT_MS               1000
#define LOGGER_WDT_MS               2500
#define LOGGER_RECEIVE_TIMEOUT_MS   1500
#define LOGGER_PROCESS_MS           350
#define HEALTH_CHECK_PERIOD_MS      200
#define SENSOR_THREAD_PRIORITY      5
#define LOGGER_THREAD_PRIORITY      6
#define HEALTH_THREAD_PRIORITY      7
#define THREAD_OPTIONS              0
#define INITIAL_VALUE               0
#define API_SUCCESS                 0
#define SIMULATE_STUCK_CONSUMER     1 /* Set to 0 to restore normal processing. */
#define CONSUMER_STALL_AFTER_MSGS    3
#define CONSUMER_STALL_MS            5000
#define QUEUE_WARN_PERCENT          75
#define PERCENT_SCALE               100
/* Zbus needs one spare buffer while cloning a publication for the logger. */
#define PUBLICATION_SPARE_BUFFERS   1
#define LOGGER_QUEUE_CAPACITY \
    (CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_POOL_SIZE - PUBLICATION_SPARE_BUFFERS)
#define QUEUE_WARN_LEVEL \
    DIV_ROUND_UP(LOGGER_QUEUE_CAPACITY * QUEUE_WARN_PERCENT, PERCENT_SCALE)

/* Successful deliveries minus messages removed by the single consumer. */
static atomic_t logger_queued;

static void health_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
    k_thread_name_set(k_current_get(), "health");

    while (true) {
        int queued = atomic_get(&logger_queued);

        if (queued >= QUEUE_WARN_LEVEL) {
            LOG_WRN("[HEALTH] logger queue=%d/%d (%d%%), threshold=%d%%",
                    queued, LOGGER_QUEUE_CAPACITY,
                    queued * PERCENT_SCALE / LOGGER_QUEUE_CAPACITY,
                    QUEUE_WARN_PERCENT);
        }
        k_sleep(K_MSEC(HEALTH_CHECK_PERIOD_MS));
    }
}

/* Called in timer interrupt context: do not sleep or block here. */
static void watchdog_timeout_cb(int channel_id, void *user_data)
{
    printk("[WATCHDOG] callback triggered: task=%s channel=%d tick=%u; resetting\n",
           (const char *)user_data, channel_id, k_uptime_get_32());
    sys_reboot(SYS_REBOOT_COLD);
}

/* ================================================================== */
/*  Shared channel message                                            */
/* ================================================================== */

struct sensor_data {
    int32_t temperature_mc;
    uint32_t timestamp_ms;
    uint8_t seq;
};

/* Forward declarations required before observer/channel definitions. */
static void display_listener_cb(const struct zbus_channel *chan);

/* ================================================================== */
/*  Observers                                                         */
/* ================================================================== */

ZBUS_LISTENER_DEFINE(display_lis, display_listener_cb);

/*
 * Logger is a message subscriber.
 * It receives message copies, not only channel notifications.
 */
ZBUS_MSG_SUBSCRIBER_DEFINE(logger_sub);

/* ================================================================== */
/*  Channel                                                           */
/* ================================================================== */

ZBUS_CHAN_DEFINE(sensor_chan, struct sensor_data,
                 NULL, NULL,
                 ZBUS_OBSERVERS(display_lis, logger_sub),
                 ZBUS_MSG_INIT(.temperature_mc = INITIAL_VALUE,
                               .timestamp_ms = INITIAL_VALUE,
                               .seq = INITIAL_VALUE));

/* ================================================================== */
/*  Listener - synchronous observer                                   */
/* ================================================================== */

static void display_listener_cb(const struct zbus_channel *chan)
{
    const struct sensor_data *msg =
        (const struct sensor_data *)zbus_chan_const_msg(chan);

    /*
     * Listener runs in publisher context.
     * Keep it short. No blocking work here.
     */
    LOG_INF("[DISPLAY-LIS] thread=%s seq=%u temp=%d mC",
            k_thread_name_get(k_current_get()),
            msg->seq,
            msg->temperature_mc);
}

/* ================================================================== */
/*  Publisher                                                         */
/* ================================================================== */

static void sensor_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    k_thread_name_set(k_current_get(), "sensor");

    int wdt_id = task_wdt_add(SENSOR_WDT_MS, watchdog_timeout_cb, "sensor");
    if (wdt_id < API_SUCCESS) {
        LOG_ERR("[SENSOR] watchdog registration failed: %d", wdt_id);
        return;
    }
    LOG_INF("[SENSOR] watchdog channel=%d timeout=%dms", wdt_id, SENSOR_WDT_MS);

    for (int i = INITIAL_VALUE; i < SENSOR_COUNT; i++) {
        struct sensor_data data = {
            .temperature_mc = SENSOR_TEMP_BASE_MC + (i * SENSOR_TEMP_STEP_MC),
            .timestamp_ms = k_uptime_get_32(),
            .seq = (uint8_t)i,
        };

        LOG_INF("[SENSOR] publish seq=%u temp=%d mC",
                data.seq,
                data.temperature_mc);

        int ret = zbus_chan_pub(&sensor_chan, &data, K_MSEC(SENSOR_PUBLISH_TIMEOUT_MS));
        if (ret == API_SUCCESS) {
            atomic_inc(&logger_queued);
        }
        if (ret == -ENOMEM) {
            /* Expected overload in this demo: drop and keep publishing. */
            LOG_WRN("[QUEUE] message pool exhausted: consumer backlog full; "
                    "seq=%u not delivered to logger ret=%d", data.seq, ret);
        } else if (ret != API_SUCCESS) {
            LOG_ERR("[SENSOR] publish failed ret=%d; watchdog will reset", ret);
            return;
        }

        /* Publication or handled overload both show publisher progress. */
        task_wdt_feed(wdt_id);
        k_msleep(SENSOR_PERIOD_MS);
    }

    /* Finite demo: stop monitoring after normal completion. */
    task_wdt_delete(wdt_id);
    LOG_INF("[SENSOR] done; watchdog channel removed");
}

/* ================================================================== */
/*  Message subscriber - logger                                       */
/* ================================================================== */

static void logger_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    k_thread_name_set(k_current_get(), "logger");

    int wdt_id = task_wdt_add(LOGGER_WDT_MS, watchdog_timeout_cb, "logger");
    if (wdt_id < API_SUCCESS) {
        LOG_ERR("[LOGGER-MSG] watchdog registration failed: %d", wdt_id);
        return;
    }
    LOG_INF("[LOGGER-MSG] watchdog channel=%d timeout=%dms", wdt_id, LOGGER_WDT_MS);

    const struct zbus_channel *chan;
    int received = INITIAL_VALUE;

    while (received < SENSOR_COUNT) {
        struct sensor_data msg;

        /*
         * Message subscribers receive a copy of the published message.
         * The slow logger will not reread the latest channel value.
         */
        int ret = zbus_sub_wait_msg(&logger_sub, &chan, &msg, K_MSEC(LOGGER_RECEIVE_TIMEOUT_MS));
        if (ret != API_SUCCESS) {
            LOG_ERR("[LOGGER-MSG] receive failed ret=%d; watchdog will reset", ret);
            return;
        }

        atomic_dec(&logger_queued);
        received++;

        LOG_INF("[LOGGER-MSG] thread=%s seq=%u temp=%d latency=%ums",
                k_thread_name_get(k_current_get()),
                msg.seq,
                msg.temperature_mc,
                k_uptime_get_32() - msg.timestamp_ms);

        if (SIMULATE_STUCK_CONSUMER && received == CONSUMER_STALL_AFTER_MSGS) {
            LOG_WRN("[LOGGER-MSG] simulating stuck consumer: sleep=%dms watchdog=%dms",
                    CONSUMER_STALL_MS, LOGGER_WDT_MS);
            /* No watchdog feed while sleeping: reset before this returns. */
            k_sleep(K_MSEC(CONSUMER_STALL_MS));
        }

        /*
         * Slow logger.
         * Message copies let it process old samples safely.
         */
        k_msleep(LOGGER_PROCESS_MS);
        /* Successful processing, including the simulated slow work. */
        task_wdt_feed(wdt_id);
    }

    task_wdt_delete(wdt_id);
    LOG_INF("[LOGGER-MSG] done received=%d; watchdog channel removed", received);
}

/* ================================================================== */
/*  Threads                                                           */
/* ================================================================== */

K_THREAD_DEFINE(sensor_thread, STACK_SIZE, sensor_thread_fn,
                NULL, NULL, NULL, SENSOR_THREAD_PRIORITY, THREAD_OPTIONS, SYS_FOREVER_MS);

K_THREAD_DEFINE(logger_thread, STACK_SIZE, logger_thread_fn,
                NULL, NULL, NULL, LOGGER_THREAD_PRIORITY, THREAD_OPTIONS, SYS_FOREVER_MS);

K_THREAD_DEFINE(health_thread, STACK_SIZE, health_thread_fn,
                NULL, NULL, NULL, HEALTH_THREAD_PRIORITY, THREAD_OPTIONS, SYS_FOREVER_MS);

/* ================================================================== */
/*  Main                                                              */
/* ================================================================== */

int main(void)
{
    /* Initialize before allowing either monitored thread to run. */
    int ret = task_wdt_init(NULL);
    if (ret != API_SUCCESS) {
        LOG_ERR("Task watchdog initialization failed: %d", ret);
        return ret;
    }

    LOG_INF("=== L4 Demo 2: Zbus Pub-Sub with Task Watchdog ===");
    LOG_INF("sensor publishes every %dms", SENSOR_PERIOD_MS);
    LOG_INF("display listener runs in publisher context");
    LOG_INF("slow logger uses message subscriber copies");

    k_thread_start(health_thread);
    k_thread_start(logger_thread);
    k_thread_start(sensor_thread);

    return API_SUCCESS;
}
