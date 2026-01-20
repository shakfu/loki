/**
 * @file context.h
 * @brief Stub header for SharedContext (not included in core Loki).
 *
 * This is a stub header that provides an empty SharedContext struct.
 * The actual audio/MIDI/Link context is part of psnd, not the core Loki editor.
 */

#ifndef SHARED_CONTEXT_H
#define SHARED_CONTEXT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Minimal stub SharedContext - no actual audio/MIDI/Link functionality */
typedef struct SharedContext {
    int dummy;  /* Placeholder to avoid empty struct */
} SharedContext;

/* Stub functions */
static inline int shared_context_init(SharedContext *ctx) {
    if (ctx) ctx->dummy = 0;
    return 0;
}
static inline void shared_context_cleanup(SharedContext *ctx) { (void)ctx; }

#ifdef __cplusplus
}
#endif

#endif /* SHARED_CONTEXT_H */
