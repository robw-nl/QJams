#ifndef AUDIO_MEMORY_H
#define AUDIO_MEMORY_H

#include "audio_nle.h"
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef QJ_MAX_TRACKS
#define QJ_MAX_TRACKS 12
#endif

// Types and Structs
typedef struct {
    float *buffer;
    uint8_t *parity_buffer;
    int track_idx;
    bool has_audio;
    uint32_t transaction_id;
    size_t canvas_frames;
    size_t v_start;
    size_t v_end;
    int v_track_idx;
    float gain;               // Structural Undo tracking
    bool is_soloed;           // Structural Undo tracking
    char name[64];            // Structural Undo tracking
    int edit_type;            // /< 0 = Memory swap, 1 = Gain Math
    float applied_multiplier; // /< For zero-RAM inverse undo
    size_t edit_start;        // /< Bounds for gain math
    size_t edit_end;          // /< Bounds for gain math
} QJUndoState;

typedef struct {
    QJUndoState *states;
    int max_capacity;
    _Atomic int head;
    _Atomic int tail;
    _Atomic int count;
    _Atomic bool is_dirty;
} QJUndoLedger;

typedef struct {
    alignas(64) float *active_buffer;
    uint8_t *parity_map;
    size_t allocated_frames;
    _Atomic float gain;
    _Atomic bool is_soloed;
    _Atomic bool has_audio;
} QJTrackBuffer;

// Global Memory Pointers
extern QJTrackBuffer master_tracks[QJ_MAX_TRACKS];
extern atomic_size_t canvas_generation;
extern atomic_size_t pristine_frames;
extern size_t pristine_allocated_frames;
extern float *clipboard_buffers[QJ_MAX_TRACKS];
extern uint8_t *clipboard_parity[QJ_MAX_TRACKS];
extern size_t clipboard_frames;
extern QJUndoLedger global_undo_ledger;

extern _Atomic uint32_t selected_tracks_mask;
extern uint32_t current_transaction_id;
extern uint32_t video_creation_trans_id;
extern uint32_t video_deletion_trans_id;

// Memory Management & NLE API
bool await_rt_thread_detach(void);
void resume_rt_thread(void);

int calculate_safe_history_capacity(size_t source_frames, int source_rate, int target_rate);
int init_exact_loop_canvas(size_t exact_frames);
int init_empty_loop_canvas(int duration_seconds);
int resize_loop_canvas_seconds(int duration_seconds);

void push_undo_state(int track_idx, uint32_t trans_id);
void push_math_undo_state(int track_idx, uint32_t trans_id, float multiplier, size_t start,
                          size_t end);
void delete_masked_multitrack_layers(uint32_t mask);

void mark_current_transaction_as_video(void);
void mark_current_transaction_as_video_deletion(void);

// Memory Helpers
float *alloc_audio_buffer(size_t frames);
void free_audio_buffer(float *ptr, size_t frames);
void safe_deferred_locked_free(float *ptr, size_t frames);

bool is_session_dirty(void);
void mark_session_saved(void);
void mark_session_dirty(void);
void evaluate_session_dirty_state(void);

void backup_track_to_ledger(int track_idx);

#endif // AUDIO_MEMORY_H
