/* async_queue.h - Generic async event queue for cross-thread communication
 *
 * Provides a thread-safe event queue for delivering asynchronous events
 * from background threads to the main thread. Uses libuv for cross-thread
 * notification.
 *
 * Features:
 * - Lock-free peek, mutex-protected push
 * - uv_async_t for waking the main thread
 * - Extensible event types with custom data
 * - Handler registration for automatic dispatch
 */

#ifndef LOKI_ASYNC_QUEUE_H
#define LOKI_ASYNC_QUEUE_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>

/* ============================================================================
 * Event Types
 * ============================================================================ */

typedef enum {
    ASYNC_EVENT_NONE = 0,

    /* Generic events */
    ASYNC_EVENT_TIMER,              /* Timer fired */
    ASYNC_EVENT_CUSTOM,             /* Custom tagged event */

    /* User-defined events start here */
    ASYNC_EVENT_USER = 16,          /* Applications can define types >= this */

    ASYNC_EVENT_TYPE_COUNT = 32     /* Max event types */
} AsyncEventType;

/* ============================================================================
 * Event Structure
 * ============================================================================ */

#define ASYNC_CUSTOM_TAG_SIZE 32
#define ASYNC_CALLBACK_NAME_SIZE 64

typedef struct AsyncEvent {
    AsyncEventType type;
    uint32_t flags;
    int64_t timestamp;              /* uv_hrtime() at push */

    union {
        /* ASYNC_EVENT_TIMER */
        struct {
            int timer_id;
            void *userdata;
        } timer;

        /* ASYNC_EVENT_CUSTOM */
        struct {
            char tag[ASYNC_CUSTOM_TAG_SIZE];
            void *data;
            size_t len;
        } custom;

        /* Generic user data (for ASYNC_EVENT_USER and above) */
        struct {
            int64_t i64[2];
            double f64[2];
            void *ptr;
        } user;
    } data;

    void *heap_data;                /* Non-NULL if data on heap, freed on pop */
} AsyncEvent;

/* ============================================================================
 * Queue Configuration
 * ============================================================================ */

#define ASYNC_QUEUE_SIZE 256        /* Must be power of 2 */
#define ASYNC_QUEUE_SIZE_MASK (ASYNC_QUEUE_SIZE - 1)
#define ASYNC_MAX_HANDLERS 32

/* ============================================================================
 * Event Handler Type
 * ============================================================================ */

typedef void (*AsyncEventHandler)(AsyncEvent *event, void *ctx);

/* ============================================================================
 * Queue Structure (opaque)
 * ============================================================================ */

typedef struct AsyncEventQueue AsyncEventQueue;

/* ============================================================================
 * Lifecycle API
 * ============================================================================ */

/**
 * Initialize the global async event queue.
 * Must be called once before using any other async_queue functions.
 *
 * @return 0 on success, -1 on error
 */
int async_queue_init(void);

/**
 * Clean up the global async event queue.
 */
void async_queue_cleanup(void);

/**
 * Get the global async event queue instance.
 *
 * @return Pointer to the global queue, or NULL if not initialized
 */
AsyncEventQueue *async_queue_global(void);

/* ============================================================================
 * Producer API (Thread-Safe)
 * ============================================================================ */

/**
 * Push an event to the queue.
 *
 * @param queue The event queue (or NULL for global)
 * @param event The event to push (copied into queue)
 * @return 0 on success, -1 if queue is full
 */
int async_queue_push(AsyncEventQueue *queue, const AsyncEvent *event);

/**
 * Push a timer event.
 *
 * @param queue The event queue (or NULL for global)
 * @param timer_id Timer identifier
 * @param userdata User-provided data
 * @return 0 on success, -1 if queue is full
 */
int async_queue_push_timer(AsyncEventQueue *queue, int timer_id, void *userdata);

/**
 * Push a custom tagged event.
 *
 * @param queue The event queue (or NULL for global)
 * @param tag Event tag (max 31 chars)
 * @param data Event data (copied to heap if len > 0)
 * @param len Data length
 * @return 0 on success, -1 if queue is full
 */
int async_queue_push_custom(AsyncEventQueue *queue, const char *tag, const void *data, size_t len);

/* ============================================================================
 * Consumer API (Main Thread Only)
 * ============================================================================ */

/**
 * Peek at the next event without removing it.
 *
 * @param queue The event queue (or NULL for global)
 * @param event Output: the next event
 * @return 0 if event available, 1 if queue empty
 */
int async_queue_peek(AsyncEventQueue *queue, AsyncEvent *event);

/**
 * Poll and remove the next event from the queue.
 *
 * @param queue The event queue (or NULL for global)
 * @param event Output: the removed event
 * @return 0 if event available, 1 if queue empty
 */
int async_queue_poll(AsyncEventQueue *queue, AsyncEvent *event);

/**
 * Pop (discard) the next event from the queue.
 *
 * @param queue The event queue (or NULL for global)
 */
void async_queue_pop(AsyncEventQueue *queue);

/**
 * Check if the queue is empty.
 *
 * @param queue The event queue (or NULL for global)
 * @return 1 if empty, 0 if events pending
 */
int async_queue_is_empty(AsyncEventQueue *queue);

/**
 * Get the number of pending events.
 *
 * @param queue The event queue (or NULL for global)
 * @return Number of events in queue
 */
int async_queue_count(AsyncEventQueue *queue);

/**
 * Dispatch all pending events to registered handlers.
 *
 * @param queue The event queue (or NULL for global)
 * @param ctx Context passed to handlers
 * @return Number of events dispatched
 */
int async_queue_dispatch_all(AsyncEventQueue *queue, void *ctx);

/* ============================================================================
 * Handler Registration
 * ============================================================================ */

/**
 * Set a handler for a specific event type.
 *
 * @param queue The event queue (or NULL for global)
 * @param type Event type to handle
 * @param handler Handler function (NULL to unregister)
 */
void async_queue_set_handler(AsyncEventQueue *queue, AsyncEventType type, AsyncEventHandler handler);

/**
 * Get the current handler for an event type.
 *
 * @param queue The event queue (or NULL for global)
 * @param type Event type
 * @return Current handler or NULL if not set
 */
AsyncEventHandler async_queue_get_handler(AsyncEventQueue *queue, AsyncEventType type);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/**
 * Free any heap-allocated data in an event.
 *
 * @param event The event to clean up
 */
void async_event_cleanup(AsyncEvent *event);

/**
 * Get the name of an event type (for debugging).
 *
 * @param type Event type
 * @return String name of the event type
 */
const char *async_event_type_name(AsyncEventType type);

#endif /* LOKI_ASYNC_QUEUE_H */
