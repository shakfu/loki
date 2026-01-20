/**
 * @file live_loop.h
 * @brief Stub header for live looping (not included in core Loki).
 *
 * Live looping requires Ableton Link and is part of psnd.
 * This stub provides no-op implementations for core Loki.
 */

#ifndef LOKI_LIVE_LOOP_H
#define LOKI_LIVE_LOOP_H

#include "loki/core.h"

/* Stub implementations - all return "not active" or no-op */
static inline int live_loop_start(editor_ctx_t *ctx, double beats) { (void)ctx; (void)beats; return -1; }
static inline void live_loop_stop(editor_ctx_t *ctx) { (void)ctx; }
static inline void live_loop_stop_buffer(int buffer_id) { (void)buffer_id; }
static inline int live_loop_is_active(editor_ctx_t *ctx) { (void)ctx; return 0; }
static inline int live_loop_is_active_buffer(int buffer_id) { (void)buffer_id; return 0; }
static inline double live_loop_get_interval(editor_ctx_t *ctx) { (void)ctx; return 0.0; }
static inline void live_loop_tick(void) {}
static inline void live_loop_shutdown(void) {}

#endif /* LOKI_LIVE_LOOP_H */
