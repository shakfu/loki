/* test_async_queue.c - Unit tests for async event queue
 *
 * Tests queue operations, thread safety, and event handling.
 */

#include "test_framework.h"
#include "async_queue.h"
#include <string.h>
#include <pthread.h>

/* Test: Queue initialization */
TEST(queue_init) {
    /* Cleanup any previous state */
    async_queue_cleanup();

    ASSERT_EQ(async_queue_init(), 0);
    ASSERT_NOT_NULL(async_queue_global());

    /* Second init should be no-op */
    ASSERT_EQ(async_queue_init(), 0);

    async_queue_cleanup();
    ASSERT_NULL(async_queue_global());
}

/* Test: Basic push and poll */
TEST(queue_push_poll) {
    async_queue_cleanup();
    ASSERT_EQ(async_queue_init(), 0);

    AsyncEventQueue *queue = async_queue_global();
    ASSERT_NOT_NULL(queue);

    /* Queue should be empty */
    ASSERT_TRUE(async_queue_is_empty(queue));
    ASSERT_EQ(async_queue_count(queue), 0);

    /* Push a timer event */
    ASSERT_EQ(async_queue_push_timer(queue, 42, NULL), 0);

    /* Queue should have one event */
    ASSERT_FALSE(async_queue_is_empty(queue));
    ASSERT_EQ(async_queue_count(queue), 1);

    /* Poll the event */
    AsyncEvent event;
    ASSERT_EQ(async_queue_poll(queue, &event), 0);
    ASSERT_EQ(event.type, ASYNC_EVENT_TIMER);
    ASSERT_EQ(event.data.timer.timer_id, 42);

    /* Queue should be empty again */
    ASSERT_TRUE(async_queue_is_empty(queue));
    ASSERT_EQ(async_queue_count(queue), 0);

    async_queue_cleanup();
}

/* Test: Multiple timer events */
TEST(queue_multiple_timers) {
    async_queue_cleanup();
    ASSERT_EQ(async_queue_init(), 0);

    AsyncEventQueue *queue = async_queue_global();

    /* Push various timer events */
    ASSERT_EQ(async_queue_push_timer(queue, 1, NULL), 0);
    ASSERT_EQ(async_queue_push_timer(queue, 2, (void *)0x1234), 0);
    ASSERT_EQ(async_queue_push_timer(queue, 3, NULL), 0);

    ASSERT_EQ(async_queue_count(queue), 3);

    /* Poll and verify each event */
    AsyncEvent event;

    ASSERT_EQ(async_queue_poll(queue, &event), 0);
    ASSERT_EQ(event.type, ASYNC_EVENT_TIMER);
    ASSERT_EQ(event.data.timer.timer_id, 1);

    ASSERT_EQ(async_queue_poll(queue, &event), 0);
    ASSERT_EQ(event.type, ASYNC_EVENT_TIMER);
    ASSERT_EQ(event.data.timer.timer_id, 2);
    ASSERT_EQ(event.data.timer.userdata, (void *)0x1234);

    ASSERT_EQ(async_queue_poll(queue, &event), 0);
    ASSERT_EQ(event.type, ASYNC_EVENT_TIMER);
    ASSERT_EQ(event.data.timer.timer_id, 3);

    ASSERT_TRUE(async_queue_is_empty(queue));

    async_queue_cleanup();
}

/* Test: Custom events with heap data */
TEST(queue_custom_events) {
    async_queue_cleanup();
    ASSERT_EQ(async_queue_init(), 0);

    AsyncEventQueue *queue = async_queue_global();

    /* Push custom event with data */
    const char *data = "hello world";
    ASSERT_EQ(async_queue_push_custom(queue, "test_tag", data, strlen(data) + 1), 0);

    AsyncEvent event;
    ASSERT_EQ(async_queue_poll(queue, &event), 0);
    ASSERT_EQ(event.type, ASYNC_EVENT_CUSTOM);
    ASSERT_STR_EQ(event.data.custom.tag, "test_tag");
    ASSERT_EQ(event.data.custom.len, strlen(data) + 1);
    ASSERT_NOT_NULL(event.data.custom.data);
    ASSERT_STR_EQ((const char *)event.data.custom.data, data);

    /* Clean up heap data */
    async_event_cleanup(&event);

    async_queue_cleanup();
}

/* Test: Custom event without data */
TEST(queue_custom_no_data) {
    async_queue_cleanup();
    ASSERT_EQ(async_queue_init(), 0);

    AsyncEventQueue *queue = async_queue_global();

    /* Push custom event without data */
    ASSERT_EQ(async_queue_push_custom(queue, "empty_tag", NULL, 0), 0);

    AsyncEvent event;
    ASSERT_EQ(async_queue_poll(queue, &event), 0);
    ASSERT_EQ(event.type, ASYNC_EVENT_CUSTOM);
    ASSERT_STR_EQ(event.data.custom.tag, "empty_tag");
    ASSERT_EQ(event.data.custom.len, 0);
    ASSERT_NULL(event.data.custom.data);

    async_queue_cleanup();
}

/* Test: Peek without consuming */
TEST(queue_peek) {
    async_queue_cleanup();
    ASSERT_EQ(async_queue_init(), 0);

    AsyncEventQueue *queue = async_queue_global();

    ASSERT_EQ(async_queue_push_timer(queue, 140, NULL), 0);

    AsyncEvent event;

    /* Peek should not consume */
    ASSERT_EQ(async_queue_peek(queue, &event), 0);
    ASSERT_EQ(event.type, ASYNC_EVENT_TIMER);
    ASSERT_EQ(async_queue_count(queue), 1);

    /* Peek again - same event */
    ASSERT_EQ(async_queue_peek(queue, &event), 0);
    ASSERT_EQ(event.type, ASYNC_EVENT_TIMER);
    ASSERT_EQ(async_queue_count(queue), 1);

    /* Pop to consume */
    async_queue_pop(queue);
    ASSERT_TRUE(async_queue_is_empty(queue));

    async_queue_cleanup();
}

/* Test: Queue full behavior */
TEST(queue_full) {
    async_queue_cleanup();
    ASSERT_EQ(async_queue_init(), 0);

    AsyncEventQueue *queue = async_queue_global();

    /* Fill the queue */
    for (int i = 0; i < ASYNC_QUEUE_SIZE - 1; i++) {
        ASSERT_EQ(async_queue_push_timer(queue, i, NULL), 0);
    }

    ASSERT_EQ(async_queue_count(queue), ASYNC_QUEUE_SIZE - 1);

    /* Queue should be full - next push should fail */
    ASSERT_EQ(async_queue_push_timer(queue, 999, NULL), -1);

    /* Drain the queue */
    AsyncEvent event;
    int count = 0;
    while (async_queue_poll(queue, &event) == 0) {
        count++;
    }
    ASSERT_EQ(count, ASYNC_QUEUE_SIZE - 1);

    async_queue_cleanup();
}

/* Test: Empty queue poll returns error */
TEST(queue_empty_poll) {
    async_queue_cleanup();
    ASSERT_EQ(async_queue_init(), 0);

    AsyncEventQueue *queue = async_queue_global();

    AsyncEvent event;
    ASSERT_EQ(async_queue_poll(queue, &event), 1);  /* 1 = empty */
    ASSERT_EQ(async_queue_peek(queue, &event), 1);

    async_queue_cleanup();
}

/* Test: NULL queue uses global */
TEST(queue_null_uses_global) {
    async_queue_cleanup();
    ASSERT_EQ(async_queue_init(), 0);

    /* Push with NULL queue should use global */
    ASSERT_EQ(async_queue_push_timer(NULL, 3, NULL), 0);

    /* Count with NULL queue should use global */
    ASSERT_EQ(async_queue_count(NULL), 1);

    /* Poll with NULL queue should use global */
    AsyncEvent event;
    ASSERT_EQ(async_queue_poll(NULL, &event), 0);
    ASSERT_EQ(event.type, ASYNC_EVENT_TIMER);

    async_queue_cleanup();
}

/* Test: Event type names */
TEST(event_type_names) {
    ASSERT_STR_EQ(async_event_type_name(ASYNC_EVENT_NONE), "NONE");
    ASSERT_STR_EQ(async_event_type_name(ASYNC_EVENT_TIMER), "TIMER");
    ASSERT_STR_EQ(async_event_type_name(ASYNC_EVENT_CUSTOM), "CUSTOM");
    ASSERT_STR_EQ(async_event_type_name(ASYNC_EVENT_USER), "USER");
    ASSERT_STR_EQ(async_event_type_name(ASYNC_EVENT_USER + 5), "USER");
}

/* Test: Handler registration */
static int g_handler_called = 0;
static AsyncEventType g_handler_event_type = ASYNC_EVENT_NONE;

static void test_handler(AsyncEvent *event, void *ctx) {
    (void)ctx;
    g_handler_called = 1;
    g_handler_event_type = event->type;
}

TEST(handler_registration) {
    async_queue_cleanup();
    ASSERT_EQ(async_queue_init(), 0);

    AsyncEventQueue *queue = async_queue_global();

    /* Register custom handler for timer events */
    async_queue_set_handler(queue, ASYNC_EVENT_TIMER, test_handler);
    ASSERT_EQ((void *)async_queue_get_handler(queue, ASYNC_EVENT_TIMER), (void *)test_handler);

    /* Push and dispatch */
    g_handler_called = 0;
    g_handler_event_type = ASYNC_EVENT_NONE;

    ASSERT_EQ(async_queue_push_timer(queue, 1, NULL), 0);
    async_queue_dispatch_all(queue, NULL);

    ASSERT_EQ(g_handler_called, 1);
    ASSERT_EQ(g_handler_event_type, ASYNC_EVENT_TIMER);

    /* Unregister handler */
    async_queue_set_handler(queue, ASYNC_EVENT_TIMER, NULL);
    ASSERT_NULL(async_queue_get_handler(queue, ASYNC_EVENT_TIMER));

    async_queue_cleanup();
}

/* Test: FIFO ordering */
TEST(queue_fifo_order) {
    async_queue_cleanup();
    ASSERT_EQ(async_queue_init(), 0);

    AsyncEventQueue *queue = async_queue_global();

    /* Push events in order */
    for (int i = 0; i < 10; i++) {
        ASSERT_EQ(async_queue_push_timer(queue, i, NULL), 0);
    }

    /* Poll should return in same order */
    AsyncEvent event;
    for (int i = 0; i < 10; i++) {
        ASSERT_EQ(async_queue_poll(queue, &event), 0);
        ASSERT_EQ(event.data.timer.timer_id, i);
    }

    async_queue_cleanup();
}

/* Test: Timestamp is set */
TEST(event_timestamp) {
    async_queue_cleanup();
    ASSERT_EQ(async_queue_init(), 0);

    AsyncEventQueue *queue = async_queue_global();

    ASSERT_EQ(async_queue_push_timer(queue, 1, NULL), 0);

    AsyncEvent event;
    ASSERT_EQ(async_queue_poll(queue, &event), 0);
    ASSERT_TRUE(event.timestamp > 0);

    async_queue_cleanup();
}

/* Test: User-defined event type */
TEST(queue_user_event) {
    async_queue_cleanup();
    ASSERT_EQ(async_queue_init(), 0);

    AsyncEventQueue *queue = async_queue_global();

    /* Create and push a user-defined event */
    AsyncEvent user_event = {
        .type = ASYNC_EVENT_USER + 1,
        .flags = 0,
        .timestamp = 0,
        .data.user = { .i64 = {123, 456}, .f64 = {1.5, 2.5}, .ptr = NULL },
        .heap_data = NULL
    };

    ASSERT_EQ(async_queue_push(queue, &user_event), 0);

    AsyncEvent event;
    ASSERT_EQ(async_queue_poll(queue, &event), 0);
    ASSERT_EQ(event.type, ASYNC_EVENT_USER + 1);
    ASSERT_EQ(event.data.user.i64[0], 123);
    ASSERT_EQ(event.data.user.i64[1], 456);
    ASSERT_TRUE(event.data.user.f64[0] > 1.4 && event.data.user.f64[0] < 1.6);

    async_queue_cleanup();
}

/* Thread function for concurrent push test */
static void *thread_push_func(void *arg) {
    int thread_id = *(int *)arg;
    AsyncEventQueue *queue = async_queue_global();

    for (int i = 0; i < 50; i++) {
        async_queue_push_timer(queue, thread_id * 100 + i, NULL);
    }

    return NULL;
}

/* Test: Concurrent pushes from multiple threads */
TEST(queue_concurrent_push) {
    async_queue_cleanup();
    ASSERT_EQ(async_queue_init(), 0);

    pthread_t threads[4];
    int thread_ids[4] = {0, 1, 2, 3};

    /* Start multiple threads pushing events */
    for (int i = 0; i < 4; i++) {
        pthread_create(&threads[i], NULL, thread_push_func, &thread_ids[i]);
    }

    /* Wait for threads to complete */
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }

    /* Verify all events were pushed (4 threads * 50 events = 200) */
    ASSERT_EQ(async_queue_count(NULL), 200);

    /* Drain the queue */
    AsyncEvent event;
    int count = 0;
    while (async_queue_poll(NULL, &event) == 0) {
        count++;
    }
    ASSERT_EQ(count, 200);

    async_queue_cleanup();
}

/* Test suite */
BEGIN_TEST_SUITE("Async Event Queue")
    RUN_TEST(queue_init);
    RUN_TEST(queue_push_poll);
    RUN_TEST(queue_multiple_timers);
    RUN_TEST(queue_custom_events);
    RUN_TEST(queue_custom_no_data);
    RUN_TEST(queue_peek);
    RUN_TEST(queue_full);
    RUN_TEST(queue_empty_poll);
    RUN_TEST(queue_null_uses_global);
    RUN_TEST(event_type_names);
    RUN_TEST(handler_registration);
    RUN_TEST(queue_fifo_order);
    RUN_TEST(event_timestamp);
    RUN_TEST(queue_user_event);
    RUN_TEST(queue_concurrent_push);
END_TEST_SUITE()
