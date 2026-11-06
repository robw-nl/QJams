#ifndef AUDIO_INTERNAL_H
#define AUDIO_INTERNAL_H
#include <stdatomic.h>
#include <stdbool.h>
extern _Atomic bool request_track_free;
extern _Atomic bool safe_to_free_track;
extern _Atomic bool stretcher_safe_to_free;
extern _Atomic bool ui_scanner_is_busy;
extern _Atomic bool is_scanning_audible;
extern _Atomic int bg_reader_count;

/**
 * @brief Attempts to acquire a shared read lock on master_tracks buffers.
 * @return true if read access is granted, false if a track free/resize is pending.
 */
static inline bool try_acquire_track_read(void) {
    if (atomic_load_explicit(&request_track_free, memory_order_acquire))
        return false;
    atomic_fetch_add_explicit(&bg_reader_count, 1, memory_order_acq_rel);
    if (atomic_load_explicit(&request_track_free, memory_order_seq_cst)) {
        atomic_fetch_sub_explicit(&bg_reader_count, 1, memory_order_acq_rel);
        return false;
    }
    return true;
}

/**
 * @brief Releases a shared read lock on master_tracks buffers.
 */
static inline void release_track_read(void) {
    atomic_fetch_sub_explicit(&bg_reader_count, 1, memory_order_release);
}
#endif
