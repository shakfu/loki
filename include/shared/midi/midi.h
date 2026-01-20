/**
 * @file midi.h
 * @brief Stub header for MIDI I/O (not included in core Loki).
 *
 * This is a stub header. The actual MIDI functionality (libremidi wrapper)
 * is part of psnd, not the core Loki editor.
 */

#ifndef SHARED_MIDI_H
#define SHARED_MIDI_H

#include <stddef.h>  /* for NULL */

#ifdef __cplusplus
extern "C" {
#endif

struct SharedContext;

/* Stub functions - return empty/error values */
static inline int shared_midi_port_count(void) { return 0; }
static inline const char* shared_midi_port_name(int index) { (void)index; return NULL; }
static inline int shared_midi_open_port(struct SharedContext *ctx, int index) {
    (void)ctx; (void)index; return -1;
}
static inline int shared_midi_open_virtual(struct SharedContext *ctx, const char *name) {
    (void)ctx; (void)name; return -1;
}
static inline void shared_midi_close(struct SharedContext *ctx) { (void)ctx; }
static inline int shared_midi_is_open(struct SharedContext *ctx) { (void)ctx; return 0; }

#ifdef __cplusplus
}
#endif

#endif /* SHARED_MIDI_H */
