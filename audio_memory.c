#define _GNU_SOURCE
#include "audio_memory.h"
#include "audio_core.h"
#include "audio_internal.h"
#include "config.h"
#include "ui_globals.h"
#include "ui_multitrack.h"
#include "video_engine.h"
#include <fcntl.h>
#include <gtk/gtk.h>
#include <pthread.h>
#include <rubberband/rubberband-c.h>

#ifndef RubberBandOptionEngineFiner
#define RubberBandOptionEngineFiner 0
#endif
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

QJTrackBuffer master_tracks[QJ_MAX_TRACKS] = {
    {.gain = 1.0f}, {.gain = 1.0f}, {.gain = 1.0f}, {.gain = 1.0f}, {.gain = 1.0f}, {.gain = 1.0f},
    {.gain = 1.0f}, {.gain = 1.0f}, {.gain = 1.0f}, {.gain = 1.0f}, {.gain = 1.0f}, {.gain = 1.0f}};

float *clipboard_buffers[QJ_MAX_TRACKS] = {NULL};
uint8_t *clipboard_parity[QJ_MAX_TRACKS] = {NULL};
size_t clipboard_frames = 0;
_Atomic uint32_t selected_tracks_mask = 1;

QJUndoLedger global_undo_ledger = {
    .states = NULL, .max_capacity = 0, .head = 0, .tail = 0, .count = 0};

atomic_size_t canvas_generation = 0;
atomic_size_t pristine_frames = 0;
size_t pristine_allocated_frames = 0;

uint32_t current_transaction_id = 0;
uint32_t video_creation_trans_id = 0;
uint32_t video_deletion_trans_id = 0;

_Atomic bool request_track_free = false;
_Atomic bool safe_to_free_track = false;
_Atomic bool stretcher_safe_to_free = false;
_Atomic bool ui_scanner_is_busy = false;
_Atomic int bg_reader_count = 0;

static pthread_mutex_t dsp_barrier_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic bool dsp_barrier_owned = false;

extern RubberBandState rt_rb_state;

extern _Atomic uint32_t dsp_heartbeat;
extern _Atomic int active_track_count;
extern _Atomic int current_recording_track;
extern _Atomic int timebase_track;
extern _Atomic bool loop_active;
extern _Atomic size_t loop_start_frame;
extern _Atomic size_t loop_end_frame;
extern atomic_size_t pristine_read_pos;
extern _Atomic bool stretcher_flush_request;
extern _Atomic bool stretcher_keep_running;

/**
 * @brief Allocates a real-time safe stereo audio buffer pinned in physical RAM.
 * Uses anonymous private mapping with pre-faulting and mlock() to guarantee
 * zero minor page faults or swap-induced stalls on the JACK DSP thread.
 * @param frames Number of stereo frames to allocate.
 * @return Pointer to the zero-initialized float buffer, or NULL on failure.
 */
float *alloc_audio_buffer(size_t frames) {
    if (frames == 0)
        return NULL;
    size_t bytes = frames * 2 * sizeof(float);

    float *ptr = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (ptr == MAP_FAILED)
        return NULL;

    (void)mlock(ptr, bytes);

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0)
        page_size = 4096;
    for (size_t i = 0; i < bytes; i += (size_t)page_size) {
        ((volatile char *)ptr)[i] = 0;
    }

    return ptr;
}

/**
 * @brief Unlocks and unmaps a real-time audio buffer allocated by alloc_audio_buffer().
 * @param ptr Pointer to the audio buffer.
 * @param frames Number of stereo frames originally allocated.
 */
void free_audio_buffer(float *ptr, size_t frames) {
    if (ptr && ptr != MAP_FAILED && frames > 0) {
        size_t bytes = frames * 2 * sizeof(float);
        (void)munlock(ptr, bytes);
        munmap(ptr, bytes);
    }
}

typedef struct {
    float *ptr;
    size_t frames;
} DeferredFreeData;

static gboolean deferred_locked_free_cb(gpointer data) {
    DeferredFreeData *df = (DeferredFreeData *)data;
    free_audio_buffer(df->ptr, df->frames);
    free(df);
    return G_SOURCE_REMOVE;
}

void safe_deferred_locked_free(float *ptr, size_t frames) {
    if (!ptr)
        return;
    DeferredFreeData *df = malloc(sizeof(DeferredFreeData));
    if (df) {
        df->ptr = ptr;
        df->frames = frames;
        g_idle_add(deferred_locked_free_cb, df);
    }
}

static gboolean deferred_free_cb(gpointer data) {
    free(data);
    return G_SOURCE_REMOVE;
}

static inline void safe_deferred_free(void *ptr) {
    if (ptr)
        g_idle_add(deferred_free_cb, ptr);
}

static void free_detached_ledger(QJUndoLedger *ledger, size_t frames) {
    (void)frames;
    if (!ledger || !ledger->states)
        return;
    for (int i = 0; i < ledger->max_capacity; i++) {
        if (ledger->states[i].buffer)
            safe_deferred_free(ledger->states[i].buffer);
        if (ledger->states[i].parity_buffer)
            safe_deferred_free(ledger->states[i].parity_buffer);
    }
    safe_deferred_free(ledger->states);
}

void free_detached_ledger_sync(QJUndoLedger *ledger, size_t frames) {
    (void)frames;
    if (!ledger || !ledger->states)
        return;
    for (int i = 0; i < ledger->max_capacity; i++) {
        if (ledger->states[i].buffer)
            free(ledger->states[i].buffer);
        if (ledger->states[i].parity_buffer)
            free(ledger->states[i].parity_buffer);
    }
    free(ledger->states);
}

/**
 * @brief Detaches the real-time JACK thread, stretcher thread, and background readers
 *        so track buffers can be safely reallocated or freed without Use-After-Free.
 * @return true if all threads safely paused/detached, false on timeout.
 */
bool await_rt_thread_detach(void) {
    pthread_mutex_lock(&dsp_barrier_mutex);
    if (atomic_load_explicit(&dsp_barrier_owned, memory_order_acquire)) {
        pthread_mutex_unlock(&dsp_barrier_mutex);
        return false;
    }
    atomic_store_explicit(&dsp_barrier_owned, true, memory_order_release);
    pthread_mutex_unlock(&dsp_barrier_mutex);

    uint32_t h1 = atomic_load_explicit(&dsp_heartbeat, memory_order_acquire);
    atomic_store_explicit(&safe_to_free_track, false, memory_order_relaxed);
    atomic_store_explicit(&stretcher_safe_to_free, false, memory_order_relaxed);
    atomic_store_explicit(&request_track_free, true, memory_order_seq_cst);

    bool stretcher_alive = atomic_load_explicit(&stretcher_keep_running, memory_order_acquire);
    int elapsed_us = 0;
    const int step_us = 200;
    const int max_timeout_us = 5000000;

    while (elapsed_us < max_timeout_us) {
        uint32_t h2 = atomic_load_explicit(&dsp_heartbeat, memory_order_acquire);
        bool rt_is_ticking = (client != NULL && (h1 != h2 || elapsed_us < 50000));
        bool rt_safe =
            rt_is_ticking ? atomic_load_explicit(&safe_to_free_track, memory_order_acquire) : true;
        bool stretcher_safe =
            stretcher_alive ? atomic_load_explicit(&stretcher_safe_to_free, memory_order_acquire)
                            : true;
        bool ui_safe = !atomic_load_explicit(&ui_scanner_is_busy, memory_order_acquire);
        bool scan_safe = !atomic_load_explicit(&is_scanning_audible, memory_order_acquire);
        bool readers_safe = (atomic_load_explicit(&bg_reader_count, memory_order_acquire) == 0);
        if (rt_safe && stretcher_safe && ui_safe && scan_safe && readers_safe)
            return true;
        usleep(step_us);
        elapsed_us += step_us;
    }

    atomic_store_explicit(&request_track_free, false, memory_order_release);
    atomic_store_explicit(&dsp_barrier_owned, false, memory_order_release);
    return false;
}

void resume_rt_thread(void) {
    atomic_store_explicit(&safe_to_free_track, false, memory_order_relaxed);
    atomic_store_explicit(&stretcher_safe_to_free, false, memory_order_relaxed);
    atomic_store_explicit(&request_track_free, false, memory_order_release);
    atomic_store_explicit(&dsp_barrier_owned, false, memory_order_release);
}

/**
 * @brief Calculates the safe undo history capacity based on available physical RAM
 *        and the 12-track monolith footprint.
 * @param source_frames Frame count of the source or target canvas.
 * @param source_rate Sample rate of the source.
 * @param target_rate Active JACK sample rate.
 * @return Number of undo states that can be safely allocated, or 0 if insufficient RAM.
 */
int calculate_safe_history_capacity(size_t source_frames, int source_rate, int target_rate) {
    if (source_rate <= 0 || target_rate <= 0)
        return 1;

    double ratio = (double)target_rate / (double)source_rate;
    size_t pristine_frames_est = (size_t)(source_frames * ratio);
    size_t track_bytes = pristine_frames_est * 2 * sizeof(float);

    size_t base_monolith_bytes = track_bytes * QJ_MAX_TRACKS;

    size_t available_ram = 0;
    FILE *meminfo = fopen("/proc/meminfo", "r");
    if (meminfo) {
        char line[256];
        while (fgets(line, sizeof(line), meminfo)) {
            if (strncmp(line, "MemAvailable:", 13) == 0) {
                long long avail_kb;
                if (sscanf(line, "MemAvailable: %lld kB", &avail_kb) == 1) {
                    available_ram = (size_t)avail_kb * 1024;
                }
                break;
            }
        }
        fclose(meminfo);
    }

    if (available_ram == 0) {
        long pages = sysconf(_SC_AVPHYS_PAGES);
        long page_size = sysconf(_SC_PAGE_SIZE);
        if (pages > 0 && page_size > 0)
            available_ram = (size_t)pages * (size_t)page_size;
    }

    size_t os_floor_bytes = 2048ULL * 1024 * 1024;

    if (available_ram <= (os_floor_bytes + base_monolith_bytes)) {
        return 0;
    }

    size_t currently_held_bytes = 0;
    for (int i = 0; i < QJ_MAX_TRACKS; i++) {
        if (master_tracks[i].active_buffer)
            currently_held_bytes += pristine_frames * 2 * sizeof(float);
    }

    currently_held_bytes += global_undo_ledger.max_capacity * (pristine_frames * 2 * sizeof(float));

    size_t long_term_ram = available_ram + currently_held_bytes;
    size_t remaining_bytes = long_term_ram - os_floor_bytes - base_monolith_bytes;

    int capacity = (int)(remaining_bytes / track_bytes);

    if (capacity > 64)
        capacity = 64;
    if (capacity < QJ_MAX_TRACKS)
        capacity = 0;

    return capacity;
}

/**
 * @brief Allocates or resets the master loop canvas and undo ledger to an exact frame count.
 * Guarantees 1:1 lifecycle synchronization between audio buffers and clip parity maps.
 * @param exact_frames The exact number of stereo frames to allocate per track.
 * @return 0 on success, -1 on engine/detach failure, -2 on insufficient RAM.
 */
int init_exact_loop_canvas(size_t exact_frames) {
    if (!client)
        return -1;
    jack_nframes_t jack_rate = jack_get_sample_rate(client);
    atomic_store_explicit(&get_transport_state()->active_sample_rate, (int)jack_rate,
                          memory_order_release);

    int capacity = calculate_safe_history_capacity(exact_frames, jack_rate, jack_rate);

    if (capacity == 0) {
        if (!await_rt_thread_detach())
            return -1;

        float *stolen_active[QJ_MAX_TRACKS] = {NULL};
        uint8_t *stolen_parity[QJ_MAX_TRACKS] = {NULL};

        for (int i = 0; i < QJ_MAX_TRACKS; i++) {
            stolen_active[i] = master_tracks[i].active_buffer;
            stolen_parity[i] = master_tracks[i].parity_map;
            master_tracks[i].active_buffer = NULL;
            master_tracks[i].parity_map = NULL;
            master_tracks[i].allocated_frames = 0;
            atomic_store_explicit(&master_tracks[i].has_audio, false, memory_order_release);
            atomic_store_explicit(&master_tracks[i].is_soloed, false, memory_order_release);
        }

        QJUndoLedger stolen_ledger = global_undo_ledger;
        global_undo_ledger.states = NULL;
        global_undo_ledger.max_capacity = 0;
        global_undo_ledger.head = 0;
        global_undo_ledger.tail = 0;
        global_undo_ledger.count = 0;

        size_t old_allocated = pristine_allocated_frames;
        atomic_store_explicit(&pristine_frames, 0, memory_order_release);
        atomic_store_explicit(&cached_audible_frames, 0, memory_order_release);
        pristine_allocated_frames = 0;
        atomic_store_explicit(&get_transport_state()->backing_track_frames, 0,
                              memory_order_release);
        atomic_store_explicit(&get_transport_state()->playback_pos, 0, memory_order_release);
        atomic_fetch_add_explicit(&canvas_generation, 1, memory_order_release);

        resume_rt_thread();

        for (int i = 0; i < QJ_MAX_TRACKS; i++) {
            if (stolen_active[i])
                free_audio_buffer(stolen_active[i], old_allocated);
            if (stolen_parity[i])
                free(stolen_parity[i]);
        }
        free_detached_ledger_sync(&stolen_ledger, 0);

        capacity = calculate_safe_history_capacity(exact_frames, jack_rate, jack_rate);
        if (capacity == 0)
            return -2;
    }

    if (exact_frames == pristine_frames && master_tracks[0].active_buffer) {
        if (!await_rt_thread_detach())
            return -1;
        for (int i = 0; i < QJ_MAX_TRACKS; i++) {
            if (master_tracks[i].active_buffer) {
                memset(master_tracks[i].active_buffer, 0, exact_frames * 2 * sizeof(float));
            }
            if (master_tracks[i].parity_map) {
                memset(master_tracks[i].parity_map, 0, exact_frames * sizeof(uint8_t));
            }
            atomic_store_explicit(&master_tracks[i].has_audio, false, memory_order_release);
            atomic_store_explicit(&master_tracks[i].is_soloed, false, memory_order_release);
        }

        global_undo_ledger.head = 0;
        global_undo_ledger.tail = 0;
        global_undo_ledger.count = 0;
        if (global_undo_ledger.states) {
            for (int i = 0; i < global_undo_ledger.max_capacity; i++) {
                global_undo_ledger.states[i].has_audio = false;
                global_undo_ledger.states[i].track_idx = -1;
            }
        }

        if (rt_rb_state)
            rubberband_reset(rt_rb_state);
        atomic_store_explicit(&pristine_read_pos, 0, memory_order_release);
        atomic_store_explicit(&cached_audible_frames, 0, memory_order_release);
        atomic_store_explicit(&get_transport_state()->playback_pos, 0, memory_order_release);
        pristine_allocated_frames = exact_frames;
        atomic_store_explicit(&get_transport_state()->backing_track_frames, exact_frames,
                              memory_order_release);
        atomic_store_explicit(&active_track_count, 1, memory_order_release);
        atomic_store_explicit(&current_recording_track, 0, memory_order_release);
        atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);

        video_start_frame = 0;
        video_end_frame = 0;
        abort_active_video_preview(false);

        mark_session_saved();
        resume_rt_thread();
        return 0;
    }

    float *stolen_active[QJ_MAX_TRACKS] = {NULL};
    uint8_t *stolen_parity[QJ_MAX_TRACKS] = {NULL};
    QJUndoLedger stolen_ledger = {0};

    if (!await_rt_thread_detach())
        return -1;
    for (int i = 0; i < QJ_MAX_TRACKS; i++) {
        stolen_active[i] = master_tracks[i].active_buffer;
        stolen_parity[i] = master_tracks[i].parity_map;
        master_tracks[i].active_buffer = NULL;
        master_tracks[i].parity_map = NULL;
        master_tracks[i].allocated_frames = 0;
    }

    stolen_ledger = global_undo_ledger;
    global_undo_ledger.states = NULL;
    global_undo_ledger.max_capacity = 0;
    global_undo_ledger.head = 0;
    global_undo_ledger.tail = 0;
    global_undo_ledger.count = 0;

    size_t old_allocated = pristine_allocated_frames;
    atomic_store_explicit(&pristine_frames, 0, memory_order_release);
    pristine_allocated_frames = 0;
    atomic_store_explicit(&get_transport_state()->backing_track_frames, 0, memory_order_release);
    atomic_fetch_add_explicit(&canvas_generation, 1, memory_order_release);
    resume_rt_thread();

    for (int i = 0; i < QJ_MAX_TRACKS; i++) {
        if (stolen_active[i])
            safe_deferred_locked_free(stolen_active[i], old_allocated);
        if (stolen_parity[i])
            safe_deferred_free(stolen_parity[i]);
    }
    free_detached_ledger(&stolen_ledger, 0);

    RubberBandState new_rb_state = rt_rb_state;
    if (!new_rb_state) {
        int rb_options = RubberBandOptionProcessRealTime | RubberBandOptionEngineFiner |
                         RubberBandOptionPitchHighQuality | RubberBandOptionPhaseIndependent |
                         RubberBandOptionWindowLong | RubberBandOptionPitchHighConsistency;
        new_rb_state = rubberband_new(jack_rate, 2, rb_options, 1.0, 1.0);
    }

    float *temp_active[QJ_MAX_TRACKS] = {NULL};
    uint8_t *temp_parity[QJ_MAX_TRACKS] = {NULL};
    for (int i = 0; i < QJ_MAX_TRACKS; i++) {
        temp_active[i] = alloc_audio_buffer(exact_frames);
        temp_parity[i] = calloc(exact_frames, sizeof(uint8_t));

        if (!temp_active[i] || !temp_parity[i]) {
            for (int j = 0; j <= i; j++) {
                if (temp_active[j])
                    free_audio_buffer(temp_active[j], exact_frames);
                if (temp_parity[j])
                    free(temp_parity[j]);
            }
            return -2;
        }
    }

    QJUndoState *temp_states = calloc(capacity, sizeof(QJUndoState));
    if (!temp_states) {
        for (int i = 0; i < QJ_MAX_TRACKS; i++) {
            free_audio_buffer(temp_active[i], exact_frames);
            free(temp_parity[i]);
        }
        return -2;
    }

    for (int i = 0; i < capacity; i++) {
        temp_states[i].track_idx = -1;
        temp_states[i].has_audio = false;
    }

    if (!await_rt_thread_detach()) {
        free(temp_states);
        for (int i = 0; i < QJ_MAX_TRACKS; i++) {
            free_audio_buffer(temp_active[i], exact_frames);
            free(temp_parity[i]);
        }
        return -1;
    }

    for (int i = 0; i < QJ_MAX_TRACKS; i++) {
        master_tracks[i].active_buffer = temp_active[i];
        master_tracks[i].parity_map = temp_parity[i];
        master_tracks[i].allocated_frames = exact_frames;
        atomic_store_explicit(&master_tracks[i].has_audio, false, memory_order_release);
        atomic_store_explicit(&master_tracks[i].is_soloed, false, memory_order_release);
    }

    global_undo_ledger.states = temp_states;
    global_undo_ledger.max_capacity = capacity;
    global_undo_ledger.head = 0;
    global_undo_ledger.tail = 0;
    global_undo_ledger.count = 0;

    rt_rb_state = new_rb_state;
    if (rt_rb_state)
        rubberband_reset(rt_rb_state);

    atomic_store_explicit(&pristine_read_pos, 0, memory_order_release);
    atomic_store_explicit(&cached_audible_frames, 0, memory_order_release);
    atomic_store_explicit(&get_transport_state()->playback_pos, 0, memory_order_release);
    atomic_store_explicit(&pristine_frames, exact_frames, memory_order_release);
    pristine_allocated_frames = exact_frames;
    atomic_store_explicit(&get_transport_state()->backing_track_frames, exact_frames,
                          memory_order_release);
    /**
     * @brief Ensure at least Track 1 is marked active upon initial canvas allocation
     * so waveform rendering and live sweep overlays immediately include Track 1.
     */
    atomic_store_explicit(&active_track_count, 1, memory_order_release);
    atomic_store_explicit(&current_recording_track, 0, memory_order_release);
    video_start_frame = 0;
    video_end_frame = 0;
    abort_active_video_preview(false);

    atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    atomic_fetch_add_explicit(&canvas_generation, 1, memory_order_release);

    mark_session_saved();

    resume_rt_thread();
    return 0;
}

/**
 * @brief Initializes an empty loop canvas for a given duration in seconds.
 * @param duration_seconds Target canvas length in seconds.
 * @return 0 on success, negative error code on failure.
 */
int init_empty_loop_canvas(int duration_seconds) {
    if (!client || duration_seconds <= 0)
        return -1;
    jack_nframes_t jack_rate = jack_get_sample_rate(client);
    return init_exact_loop_canvas((size_t)jack_rate * (size_t)duration_seconds);
}

/**
 * @brief Resizes the multi-track audio canvas to the specified duration in seconds.
 *        Preserves any larger pre-crop undo state allocations and verifies all heap allocations.
 * @param duration_seconds Target canvas length in seconds.
 * @return 0 on success, -1 on detach failure, -2 on insufficient RAM.
 */
int resize_loop_canvas_seconds(int duration_seconds) {
    if (!client || duration_seconds <= 0)
        return -1;
    jack_nframes_t jack_rate =
        atomic_load_explicit(&get_transport_state()->active_sample_rate, memory_order_acquire);
    if (jack_rate <= 0)
        jack_rate = 48000;
    size_t new_exact_frames = (size_t)jack_rate * (size_t)duration_seconds;
    if (new_exact_frames == pristine_frames && new_exact_frames == pristine_allocated_frames)
        return 0;
    int new_capacity = calculate_safe_history_capacity(new_exact_frames, jack_rate, jack_rate);
    if (new_capacity == 0)
        return -2;
    size_t frames_to_copy =
        (new_exact_frames < pristine_frames) ? new_exact_frames : pristine_frames;
    float *new_active[QJ_MAX_TRACKS] = {NULL};
    uint8_t *new_parity[QJ_MAX_TRACKS] = {NULL};
    for (int i = 0; i < QJ_MAX_TRACKS; i++) {
        new_active[i] = alloc_audio_buffer(new_exact_frames);
        new_parity[i] = calloc(new_exact_frames, sizeof(uint8_t));
        if (!new_active[i] || !new_parity[i]) {
            for (int j = 0; j <= i; j++) {
                if (new_active[j])
                    free_audio_buffer(new_active[j], new_exact_frames);
                if (new_parity[j])
                    free(new_parity[j]);
            }
            return -2;
        }
    }
    QJUndoState *new_states = calloc(new_capacity, sizeof(QJUndoState));
    if (!new_states) {
        for (int i = 0; i < QJ_MAX_TRACKS; i++) {
            free_audio_buffer(new_active[i], new_exact_frames);
            free(new_parity[i]);
        }
        return -2;
    }
    for (int i = 0; i < new_capacity; i++) {
        new_states[i].track_idx = -1;
        new_states[i].has_audio = false;
    }
    int target_count = global_undo_ledger.count;
    int shrink_diff = target_count - new_capacity;
    if (shrink_diff > 0)
        target_count = new_capacity;
    int new_head = global_undo_ledger.head - (shrink_diff > 0 ? shrink_diff : 0);
    if (new_head < 0)
        new_head = 0;
    float current_speed =
        atomic_load_explicit(&get_transport_state()->playback_speed, memory_order_relaxed);
    if (current_speed <= 0.0f)
        current_speed = 1.0f;
    for (int i = 0; i < target_count; i++) {
        int old_idx = (global_undo_ledger.tail + (shrink_diff > 0 ? shrink_diff : 0) + i) %
                      global_undo_ledger.max_capacity;
        size_t old_state_pristine =
            (size_t)((double)global_undo_ledger.states[old_idx].canvas_frames *
                     (double)current_speed);
        if (old_state_pristine > pristine_allocated_frames)
            old_state_pristine = pristine_allocated_frames;
        size_t state_alloc_frames =
            (new_exact_frames > old_state_pristine) ? new_exact_frames : old_state_pristine;
        if (global_undo_ledger.states[old_idx].buffer) {
            new_states[i].buffer = calloc(state_alloc_frames * 2, sizeof(float));
            if (!new_states[i].buffer)
                goto resize_undo_alloc_fail;
        }
        if (global_undo_ledger.states[old_idx].parity_buffer) {
            new_states[i].parity_buffer = calloc(state_alloc_frames, sizeof(uint8_t));
            if (!new_states[i].parity_buffer)
                goto resize_undo_alloc_fail;
        }
    }
    if (!await_rt_thread_detach()) {
    resize_undo_alloc_fail:
        for (int i = 0; i < target_count; i++) {
            free(new_states[i].buffer);
            free(new_states[i].parity_buffer);
        }
        free(new_states);
        for (int i = 0; i < QJ_MAX_TRACKS; i++) {
            free_audio_buffer(new_active[i], new_exact_frames);
            free(new_parity[i]);
        }
        return -1;
    }
    QJUndoLedger old_ledger = global_undo_ledger;
    for (int i = 0; i < target_count; i++) {
        int old_idx = (global_undo_ledger.tail + (shrink_diff > 0 ? shrink_diff : 0) + i) %
                      global_undo_ledger.max_capacity;
        size_t old_state_pristine =
            (size_t)((double)global_undo_ledger.states[old_idx].canvas_frames *
                     (double)current_speed);
        if (old_state_pristine > pristine_allocated_frames)
            old_state_pristine = pristine_allocated_frames;
        size_t state_copy_frames =
            (old_state_pristine > frames_to_copy) ? old_state_pristine : frames_to_copy;
        if (state_copy_frames > pristine_allocated_frames)
            state_copy_frames = pristine_allocated_frames;
        if (global_undo_ledger.states[old_idx].buffer && new_states[i].buffer) {
            memcpy(new_states[i].buffer, global_undo_ledger.states[old_idx].buffer,
                   state_copy_frames * 2 * sizeof(float));
        }
        if (global_undo_ledger.states[old_idx].parity_buffer && new_states[i].parity_buffer) {
            memcpy(new_states[i].parity_buffer, global_undo_ledger.states[old_idx].parity_buffer,
                   state_copy_frames * sizeof(uint8_t));
        }
        new_states[i].track_idx = global_undo_ledger.states[old_idx].track_idx;
        new_states[i].has_audio = global_undo_ledger.states[old_idx].has_audio;
        new_states[i].transaction_id = global_undo_ledger.states[old_idx].transaction_id;
        new_states[i].canvas_frames = global_undo_ledger.states[old_idx].canvas_frames;
        new_states[i].v_start = global_undo_ledger.states[old_idx].v_start;
        new_states[i].v_end = global_undo_ledger.states[old_idx].v_end;
        new_states[i].v_track_idx = global_undo_ledger.states[old_idx].v_track_idx;
        new_states[i].gain = global_undo_ledger.states[old_idx].gain;
        new_states[i].is_soloed = global_undo_ledger.states[old_idx].is_soloed;
        strncpy(new_states[i].name, global_undo_ledger.states[old_idx].name, 63);
        new_states[i].name[63] = '\0';
        new_states[i].edit_type = global_undo_ledger.states[old_idx].edit_type;
        new_states[i].applied_multiplier = global_undo_ledger.states[old_idx].applied_multiplier;
        new_states[i].edit_start = global_undo_ledger.states[old_idx].edit_start;
        new_states[i].edit_end = global_undo_ledger.states[old_idx].edit_end;
    }
    global_undo_ledger.states = new_states;
    global_undo_ledger.max_capacity = new_capacity;
    global_undo_ledger.head = new_head;
    global_undo_ledger.tail = 0;
    global_undo_ledger.count = target_count;
    for (int i = 0; i < QJ_MAX_TRACKS; i++) {
        if (master_tracks[i].active_buffer) {
            memcpy(new_active[i], master_tracks[i].active_buffer,
                   frames_to_copy * 2 * sizeof(float));
            safe_deferred_locked_free(master_tracks[i].active_buffer,
                                      master_tracks[i].allocated_frames);
        }
        if (master_tracks[i].parity_map) {
            memcpy(new_parity[i], master_tracks[i].parity_map, frames_to_copy * sizeof(uint8_t));
            safe_deferred_free(master_tracks[i].parity_map);
        }
        master_tracks[i].active_buffer = new_active[i];
        master_tracks[i].parity_map = new_parity[i];
        master_tracks[i].allocated_frames = new_exact_frames;
    }
    pristine_allocated_frames = new_exact_frames;
    atomic_store_explicit(&pristine_frames, new_exact_frames, memory_order_release);
    size_t new_bt_frames = (size_t)((double)new_exact_frames / (double)current_speed);
    atomic_store_explicit(&get_transport_state()->backing_track_frames, new_bt_frames,
                          memory_order_release);
    size_t current_pos =
        atomic_load_explicit(&get_transport_state()->playback_pos, memory_order_acquire);
    if (current_pos > new_exact_frames) {
        atomic_store_explicit(&get_transport_state()->playback_pos, new_exact_frames,
                              memory_order_release);
    }
    atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    resume_rt_thread();
    free_detached_ledger(&old_ledger, frames_to_copy);
    return 0;
}

/**
 * @brief Deletes (wipes) all tracks specified in the bitmask, pushing undo states
 *        and stashing video if the video track is among the wiped layers.
 * @param mask Bitmask of tracks [0 .. QJ_MAX_TRACKS - 1] to clear.
 */
void delete_masked_multitrack_layers(uint32_t mask) {
    if (mask == 0)
        return;
    int current_rec = atomic_load_explicit(&current_recording_track, memory_order_acquire);
    int new_focus = current_rec;
    while (new_focus > 0 && (mask & (1 << new_focus))) {
        new_focus--;
    }
    bool any_audio = false;
    for (int i = 0; i < QJ_MAX_TRACKS; i++) {
        if ((mask & (1 << i)) &&
            atomic_load_explicit(&master_tracks[i].has_audio, memory_order_acquire)) {
            any_audio = true;
            break;
        }
    }
    if (!any_audio) {
        for (int i = 0; i < QJ_MAX_TRACKS; i++) {
            if (mask & (1 << i)) {
                atomic_store_explicit(&master_tracks[i].gain, 1.0f, memory_order_release);
                atomic_store_explicit(&master_tracks[i].is_soloed, false, memory_order_release);
                char default_name[64];
                snprintf(default_name, sizeof(default_name), "Track %d", i + 1);
                set_multitrack_track_name(i, default_name);
            }
        }
        atomic_store_explicit(&current_recording_track, new_focus, memory_order_release);
        atomic_store_explicit(&selected_tracks_mask, (1 << new_focus), memory_order_release);
        return;
    }
    if (!await_rt_thread_detach())
        return;
    current_transaction_id++;
    bool video_wiped = false;
    for (int i = 0; i < QJ_MAX_TRACKS; i++) {
        if (mask & (1 << i)) {
            push_undo_state(i, current_transaction_id);
            if (master_tracks[i].active_buffer) {
                memset(master_tracks[i].active_buffer, 0, pristine_frames * 2 * sizeof(float));
                if (master_tracks[i].parity_map) {
                    memset(master_tracks[i].parity_map, 0, pristine_frames * sizeof(uint8_t));
                }
            }
            atomic_store_explicit(&master_tracks[i].gain, 1.0f, memory_order_release);
            atomic_store_explicit(&master_tracks[i].is_soloed, false, memory_order_release);
            atomic_store_explicit(&master_tracks[i].has_audio, false, memory_order_release);
            char default_name[64];
            snprintf(default_name, sizeof(default_name), "Track %d", i + 1);
            set_multitrack_track_name(i, default_name);
            if (video_track_index == i) {
                video_wiped = true;
                video_start_frame = 0;
                video_end_frame = 0;
                video_track_index = -1;
                mark_current_transaction_as_video_deletion();
            }
        }
    }
    atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    atomic_store_explicit(&current_recording_track, new_focus, memory_order_release);
    atomic_store_explicit(&selected_tracks_mask, (1 << new_focus), memory_order_release);
    int highest_active = 0;
    for (int i = QJ_MAX_TRACKS - 1; i >= 0; i--) {
        if (atomic_load_explicit(&master_tracks[i].has_audio, memory_order_acquire)) {
            highest_active = i;
            break;
        }
    }
    int new_active_count = highest_active + 1;
    if (new_focus >= new_active_count)
        new_active_count = new_focus + 1;
    atomic_store_explicit(&active_track_count, new_active_count, memory_order_release);
    resume_rt_thread();
    if (video_wiped) {
        abort_active_video_preview(true);
        force_clear_mkv_video_preview();
    }
}

/**
 * @brief Pushes a full buffer snapshot of track_idx onto the circular undo ledger.
 *        Pre-allocates/reallocates buffers before advancing ledger indices to prevent
 *        dangling or undersized buffers on out-of-memory conditions.
 * @param track_idx Track index [0 .. QJ_MAX_TRACKS - 1].
 * @param trans_id Transaction ID grouping multi-track edits.
 */
void push_undo_state(int track_idx, uint32_t trans_id) {
    if (global_undo_ledger.max_capacity <= 0 || !global_undo_ledger.states)
        return;
    mark_session_dirty();
    int next_count = (global_undo_ledger.count > global_undo_ledger.head)
                         ? global_undo_ledger.head
                         : global_undo_ledger.count;
    int next_tail = global_undo_ledger.tail;
    if (next_count == global_undo_ledger.max_capacity) {
        uint32_t drop_id = global_undo_ledger.states[next_tail].transaction_id;
        while (next_count > 0 && global_undo_ledger.states[next_tail].transaction_id == drop_id) {
            next_tail = (next_tail + 1) % global_undo_ledger.max_capacity;
            next_count--;
        }
    }
    int target_idx = (next_tail + next_count) % global_undo_ledger.max_capacity;
    QJUndoState *state = &global_undo_ledger.states[target_idx];
    if (pristine_frames > 0) {
        size_t available_ram = 0;
        FILE *meminfo = fopen("/proc/meminfo", "r");
        if (meminfo) {
            char line[256];
            while (fgets(line, sizeof(line), meminfo)) {
                if (strncmp(line, "MemAvailable:", 13) == 0) {
                    long long avail_kb;
                    if (sscanf(line, "MemAvailable: %lld kB", &avail_kb) == 1) {
                        available_ram = (size_t)avail_kb * 1024;
                    }
                    break;
                }
            }
            fclose(meminfo);
        }
        size_t required_bytes = pristine_frames * 2 * sizeof(float);
        if (available_ram < required_bytes + (1024ULL * 1024 * 1024)) {
            log_and_notify(1, "<span foreground='#ff4444'><b>Memory low: Action applied, but Undo "
                              "history was not saved.</b></span>");
            return;
        }
        float *new_buf = realloc(state->buffer, required_bytes);
        if (!new_buf) {
            log_and_notify(1, "<span foreground='#ff4444'><b>Memory low: Action applied, but Undo "
                              "history was not saved.</b></span>");
            return;
        }
        state->buffer = new_buf;
        uint8_t *new_p_buf = realloc(state->parity_buffer, pristine_frames * sizeof(uint8_t));
        if (!new_p_buf) {
            log_and_notify(1, "<span foreground='#ff4444'><b>Memory low: Action applied, but Undo "
                              "history was not saved.</b></span>");
            return;
        }
        state->parity_buffer = new_p_buf;
    } else {
        if (state->buffer) {
            free(state->buffer);
            state->buffer = NULL;
        }
        if (state->parity_buffer) {
            free(state->parity_buffer);
            state->parity_buffer = NULL;
        }
    }
    global_undo_ledger.tail = next_tail;
    global_undo_ledger.count = next_count;
    if (state->buffer && master_tracks[track_idx].active_buffer) {
        memcpy(state->buffer, master_tracks[track_idx].active_buffer,
               pristine_frames * 2 * sizeof(float));
    }
    if (state->parity_buffer && master_tracks[track_idx].parity_map) {
        memcpy(state->parity_buffer, master_tracks[track_idx].parity_map,
               pristine_frames * sizeof(uint8_t));
    }
    state->track_idx = track_idx;
    state->has_audio =
        atomic_load_explicit(&master_tracks[track_idx].has_audio, memory_order_acquire);
    state->transaction_id = trans_id;
    state->canvas_frames =
        atomic_load_explicit(&get_transport_state()->backing_track_frames, memory_order_acquire);
    if (state->canvas_frames == 0) {
        state->canvas_frames = pristine_frames;
    }
    state->gain = atomic_load_explicit(&master_tracks[track_idx].gain, memory_order_acquire);
    state->is_soloed =
        atomic_load_explicit(&master_tracks[track_idx].is_soloed, memory_order_acquire);
    get_multitrack_track_name(track_idx, state->name);
    state->v_start = video_start_frame;
    state->v_end = video_end_frame;
    state->v_track_idx = video_track_index;
    state->edit_type = 0;
    state->applied_multiplier = 1.0f;
    state->edit_start = 0;
    state->edit_end = 0;
    global_undo_ledger.count++;
    global_undo_ledger.head = global_undo_ledger.count;
}

/**
 * @brief Pushes a zero-RAM mathematical state into the ledger.
 * By storing the operation rather than the payload, memory bandwidth is perfectly conserved.
 * @param track_idx Target track index.
 * @param trans_id Transaction grouping ID.
 * @param multiplier The applied float multiplier.
 * @param start The edit start boundary.
 * @param end The edit end boundary.
 */
void push_math_undo_state(int track_idx, uint32_t trans_id, float multiplier, size_t start,
                          size_t end) {
    if (global_undo_ledger.max_capacity <= 0 || !global_undo_ledger.states)
        return;

    mark_session_dirty();

    if (global_undo_ledger.count > global_undo_ledger.head) {
        global_undo_ledger.count = global_undo_ledger.head;
    }

    if (global_undo_ledger.count == global_undo_ledger.max_capacity) {
        uint32_t drop_id = global_undo_ledger.states[global_undo_ledger.tail].transaction_id;
        while (global_undo_ledger.count > 0 &&
               global_undo_ledger.states[global_undo_ledger.tail].transaction_id == drop_id) {
            global_undo_ledger.tail =
                (global_undo_ledger.tail + 1) % global_undo_ledger.max_capacity;
            global_undo_ledger.count--;
        }
    }

    int target_idx =
        (global_undo_ledger.tail + global_undo_ledger.count) % global_undo_ledger.max_capacity;
    QJUndoState *state = &global_undo_ledger.states[target_idx];

    if (state->buffer) {
        free(state->buffer);
        state->buffer = NULL;
    }
    if (state->parity_buffer) {
        free(state->parity_buffer);
        state->parity_buffer = NULL;
    }

    state->track_idx = track_idx;
    state->has_audio =
        atomic_load_explicit(&master_tracks[track_idx].has_audio, memory_order_acquire);
    state->transaction_id = trans_id;
    state->canvas_frames =
        atomic_load_explicit(&get_transport_state()->backing_track_frames, memory_order_acquire);

    state->gain = atomic_load_explicit(&master_tracks[track_idx].gain, memory_order_acquire);
    state->is_soloed =
        atomic_load_explicit(&master_tracks[track_idx].is_soloed, memory_order_acquire);
    get_multitrack_track_name(track_idx, state->name);

    state->v_start = video_start_frame;
    state->v_end = video_end_frame;
    state->v_track_idx = video_track_index;

    state->edit_type = 1;
    state->applied_multiplier = multiplier;
    state->edit_start = start;
    state->edit_end = end;

    global_undo_ledger.count++;
    global_undo_ledger.head = global_undo_ledger.count;
}

_Atomic uint32_t saved_transaction_id = 0;

static uint32_t get_current_transaction(void) {
    if (global_undo_ledger.count == 0 || global_undo_ledger.head == 0)
        return 0;
    int idx =
        (global_undo_ledger.tail + global_undo_ledger.head - 1) % global_undo_ledger.max_capacity;
    return global_undo_ledger.states[idx].transaction_id;
}

void evaluate_session_dirty_state(void) {
    if (get_current_transaction() ==
        atomic_load_explicit(&saved_transaction_id, memory_order_acquire)) {
        atomic_store_explicit(&global_undo_ledger.is_dirty, false, memory_order_release);
    } else {
        atomic_store_explicit(&global_undo_ledger.is_dirty, true, memory_order_release);
    }
}

bool is_session_dirty(void) {
    return atomic_load_explicit(&global_undo_ledger.is_dirty, memory_order_acquire);
}

void mark_session_saved(void) {
    atomic_store_explicit(&saved_transaction_id, get_current_transaction(), memory_order_release);
    atomic_store_explicit(&global_undo_ledger.is_dirty, false, memory_order_release);
}

void mark_session_dirty(void) {
    atomic_store_explicit(&global_undo_ledger.is_dirty, true, memory_order_release);
}

void backup_track_to_ledger(int track_idx) {
    if (track_idx >= 0 && track_idx < QJ_MAX_TRACKS) {
        current_transaction_id++;
        push_undo_state(track_idx, current_transaction_id);
    }
}

void mark_current_transaction_as_video(void) {
    video_creation_trans_id = current_transaction_id;
    video_deletion_trans_id = 0;
}

void mark_current_transaction_as_video_deletion(void) {
    video_deletion_trans_id = current_transaction_id;
}
