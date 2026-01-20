/* async_queue.c - Generic async event queue implementation
 *
 * Thread-safe ring buffer with libuv cross-thread notification.
 */

#include "async_queue.h"
#include <uv.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Queue Structure
 * ============================================================================ */

struct AsyncEventQueue {
    /* Ring buffer */
    AsyncEvent events[ASYNC_QUEUE_SIZE];
    _Atomic uint32_t head;          /* Consumer reads from here */
    _Atomic uint32_t tail;          /* Producer writes here */

    /* Cross-thread notification */
    uv_async_t wakeup;
    uv_mutex_t mutex;

    /* Handler dispatch table */
    AsyncEventHandler handlers[ASYNC_MAX_HANDLERS];

    /* Initialization state */
    int initialized;

    /* libuv loop (borrowed, not owned) */
    uv_loop_t *loop;
};

/* Global queue instance */
static AsyncEventQueue g_queue;

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

static inline AsyncEventQueue *resolve_queue(AsyncEventQueue *queue) {
    return queue ? queue : (g_queue.initialized ? &g_queue : NULL);
}

/* Wake callback - does nothing, just wakes the event loop */
static void on_queue_wakeup(uv_async_t *handle) {
    (void)handle;
    /* The wakeup signal itself is sufficient - main thread will poll queue */
}

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

int async_queue_init(void) {
    if (g_queue.initialized) {
        return 0;  /* Already initialized */
    }

    memset(&g_queue, 0, sizeof(g_queue));

    /* Initialize atomic counters */
    atomic_store(&g_queue.head, 0);
    atomic_store(&g_queue.tail, 0);

    /* Initialize mutex */
    if (uv_mutex_init(&g_queue.mutex) != 0) {
        return -1;
    }

    /* Get or create default loop */
    g_queue.loop = uv_default_loop();
    if (!g_queue.loop) {
        uv_mutex_destroy(&g_queue.mutex);
        return -1;
    }

    /* Initialize async handle for cross-thread notification */
    if (uv_async_init(g_queue.loop, &g_queue.wakeup, on_queue_wakeup) != 0) {
        uv_mutex_destroy(&g_queue.mutex);
        return -1;
    }

    g_queue.initialized = 1;

    return 0;
}

void async_queue_cleanup(void) {
    if (!g_queue.initialized) {
        return;
    }

    /* Drain queue and free any heap data */
    AsyncEvent event;
    while (async_queue_poll(&g_queue, &event) == 0) {
        async_event_cleanup(&event);
    }

    /* Close async handle */
    uv_close((uv_handle_t *)&g_queue.wakeup, NULL);

    /* Run loop once to process close callbacks */
    uv_run(g_queue.loop, UV_RUN_NOWAIT);

    uv_mutex_destroy(&g_queue.mutex);

    g_queue.initialized = 0;
}

AsyncEventQueue *async_queue_global(void) {
    return g_queue.initialized ? &g_queue : NULL;
}

/* ============================================================================
 * Producer API (Thread-Safe)
 * ============================================================================ */

int async_queue_push(AsyncEventQueue *queue, const AsyncEvent *event) {
    queue = resolve_queue(queue);
    if (!queue || !event) {
        return -1;
    }

    uv_mutex_lock(&queue->mutex);

    uint32_t tail = atomic_load(&queue->tail);
    uint32_t next = (tail + 1) & ASYNC_QUEUE_SIZE_MASK;

    if (next == atomic_load(&queue->head)) {
        /* Queue full */
        uv_mutex_unlock(&queue->mutex);
        return -1;
    }

    /* Copy event into queue */
    queue->events[tail] = *event;

    /* Set timestamp if not already set */
    if (queue->events[tail].timestamp == 0) {
        queue->events[tail].timestamp = (int64_t)uv_hrtime();
    }

    atomic_store(&queue->tail, next);

    uv_mutex_unlock(&queue->mutex);

    /* Wake main thread */
    uv_async_send(&queue->wakeup);

    return 0;
}

int async_queue_push_timer(AsyncEventQueue *queue, int timer_id, void *userdata) {
    AsyncEvent event = {
        .type = ASYNC_EVENT_TIMER,
        .flags = 0,
        .timestamp = 0,  /* Will be set by push */
        .data.timer = { .timer_id = timer_id, .userdata = userdata },
        .heap_data = NULL
    };
    return async_queue_push(queue, &event);
}

int async_queue_push_custom(AsyncEventQueue *queue, const char *tag, const void *data, size_t len) {
    AsyncEvent event = {
        .type = ASYNC_EVENT_CUSTOM,
        .flags = 0,
        .timestamp = 0,
        .heap_data = NULL
    };

    /* Copy tag (truncate if needed) */
    if (tag) {
        strncpy(event.data.custom.tag, tag, ASYNC_CUSTOM_TAG_SIZE - 1);
        event.data.custom.tag[ASYNC_CUSTOM_TAG_SIZE - 1] = '\0';
    } else {
        event.data.custom.tag[0] = '\0';
    }

    /* Copy data to heap if provided */
    if (data && len > 0) {
        event.heap_data = malloc(len);
        if (!event.heap_data) {
            return -1;
        }
        memcpy(event.heap_data, data, len);
        event.data.custom.data = event.heap_data;
        event.data.custom.len = len;
    } else {
        event.data.custom.data = NULL;
        event.data.custom.len = 0;
    }

    int result = async_queue_push(queue, &event);
    if (result != 0 && event.heap_data) {
        free(event.heap_data);
    }
    return result;
}

/* ============================================================================
 * Consumer API (Main Thread Only)
 * ============================================================================ */

int async_queue_peek(AsyncEventQueue *queue, AsyncEvent *event) {
    queue = resolve_queue(queue);
    if (!queue || !event) {
        return 1;
    }

    uint32_t head = atomic_load(&queue->head);
    uint32_t tail = atomic_load(&queue->tail);

    if (head == tail) {
        return 1;  /* Empty */
    }

    *event = queue->events[head];
    return 0;
}

int async_queue_poll(AsyncEventQueue *queue, AsyncEvent *event) {
    queue = resolve_queue(queue);
    if (!queue || !event) {
        return 1;
    }

    uint32_t head = atomic_load(&queue->head);
    uint32_t tail = atomic_load(&queue->tail);

    if (head == tail) {
        return 1;  /* Empty */
    }

    *event = queue->events[head];
    atomic_store(&queue->head, (head + 1) & ASYNC_QUEUE_SIZE_MASK);

    return 0;
}

void async_queue_pop(AsyncEventQueue *queue) {
    queue = resolve_queue(queue);
    if (!queue) {
        return;
    }

    uint32_t head = atomic_load(&queue->head);
    uint32_t tail = atomic_load(&queue->tail);

    if (head == tail) {
        return;  /* Empty */
    }

    /* Free any heap data */
    async_event_cleanup(&queue->events[head]);

    atomic_store(&queue->head, (head + 1) & ASYNC_QUEUE_SIZE_MASK);
}

int async_queue_is_empty(AsyncEventQueue *queue) {
    queue = resolve_queue(queue);
    if (!queue) {
        return 1;
    }

    return atomic_load(&queue->head) == atomic_load(&queue->tail) ? 1 : 0;
}

int async_queue_count(AsyncEventQueue *queue) {
    queue = resolve_queue(queue);
    if (!queue) {
        return 0;
    }

    uint32_t head = atomic_load(&queue->head);
    uint32_t tail = atomic_load(&queue->tail);

    return (int)((tail - head) & ASYNC_QUEUE_SIZE_MASK);
}

int async_queue_dispatch_all(AsyncEventQueue *queue, void *ctx) {
    queue = resolve_queue(queue);
    if (!queue) {
        return 0;
    }

    int count = 0;
    AsyncEvent event;

    while (async_queue_poll(queue, &event) == 0) {
        if (event.type > ASYNC_EVENT_NONE && event.type < ASYNC_MAX_HANDLERS) {
            AsyncEventHandler handler = queue->handlers[event.type];
            if (handler) {
                handler(&event, ctx);
            }
        }
        async_event_cleanup(&event);
        count++;
    }

    return count;
}

/* ============================================================================
 * Handler Registration
 * ============================================================================ */

void async_queue_set_handler(AsyncEventQueue *queue, AsyncEventType type, AsyncEventHandler handler) {
    queue = resolve_queue(queue);
    if (!queue || type <= ASYNC_EVENT_NONE || type >= ASYNC_MAX_HANDLERS) {
        return;
    }

    queue->handlers[type] = handler;
}

AsyncEventHandler async_queue_get_handler(AsyncEventQueue *queue, AsyncEventType type) {
    queue = resolve_queue(queue);
    if (!queue || type <= ASYNC_EVENT_NONE || type >= ASYNC_MAX_HANDLERS) {
        return NULL;
    }

    return queue->handlers[type];
}

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

void async_event_cleanup(AsyncEvent *event) {
    if (!event) {
        return;
    }

    if (event->heap_data) {
        free(event->heap_data);
        event->heap_data = NULL;
    }

    /* Clear custom data pointer if it was pointing to heap */
    if (event->type == ASYNC_EVENT_CUSTOM) {
        event->data.custom.data = NULL;
        event->data.custom.len = 0;
    }
}

const char *async_event_type_name(AsyncEventType type) {
    switch (type) {
        case ASYNC_EVENT_NONE:   return "NONE";
        case ASYNC_EVENT_TIMER:  return "TIMER";
        case ASYNC_EVENT_CUSTOM: return "CUSTOM";
        default:
            if (type >= ASYNC_EVENT_USER && type < ASYNC_EVENT_TYPE_COUNT) {
                return "USER";
            }
            return "UNKNOWN";
    }
}
