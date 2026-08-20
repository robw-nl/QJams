#ifndef QJAMS_RINGBUFFER_H
#define QJAMS_RINGBUFFER_H

#include <stddef.h>
#include <stdatomic.h>
#include <stdalign.h>
#include <stdlib.h>

// Align structures to 64 bytes to match the typical L1 cache line size
// This physically separates the read and write indices in the CPU cache,
// eliminating false-sharing latency when the encoder thread reads from the buffer.
#define CACHE_LINE_SIZE 64

typedef struct {
    float mix_l;
    float mix_r;
    int64_t pts;
} AudioFrame;

typedef struct {
    AudioFrame* buffer;
    size_t capacity; // Must be a power of 2

    alignas(CACHE_LINE_SIZE) atomic_size_t write_index;
    alignas(CACHE_LINE_SIZE) atomic_size_t read_index;
} SPSC_Audio_Queue;

/**
 * @brief Opcodes for lock-free commands dispatched from the GTK UI thread to the JACK RT thread.
 * Legacy O(N) array mutation commands have been purged to enforce strict RT safety.
 */
typedef enum {
    CMD_NONE = 0,
    CMD_NEXT_TRACK
} EngineCommandType;

// Initialize the queue (called once during setup, before RT thread starts)
/**
 * @brief Initializes the lock-free single-producer single-consumer audio queue.
 * @param q Pointer to the queue structure.
 * @param capacity The maximum number of frames (must be a power of two).
 * @return void
 */
static inline void init_spsc_queue(SPSC_Audio_Queue* q, size_t capacity) {
    q->buffer = (AudioFrame*)calloc(capacity, sizeof(AudioFrame));
    q->capacity = capacity;
    atomic_init(&q->write_index, 0);
}

// Producer (Called strictly by the JACK process() callback)
/**
 * @brief Acquires a pointer to the next available ringbuffer slot to write data directly without copying.
 * @param q Pointer to the queue structure.
 * @return Pointer to an AudioFrame, or NULL if the buffer is full.
 */
static inline AudioFrame* acquire_audio_frame(SPSC_Audio_Queue* q) {
    size_t current_write = atomic_load_explicit(&q->write_index, memory_order_relaxed);
    size_t current_read = atomic_load_explicit(&q->read_index, memory_order_acquire);

    if (current_write - current_read >= q->capacity) return NULL;
    return &q->buffer[current_write & (q->capacity - 1)];
}

/**
 * @brief Commits the previously acquired audio frame to the queue.
 * @param q Pointer to the queue structure.
 * @return void
 */
static inline void commit_audio_frame(SPSC_Audio_Queue* q) {
    size_t current_write = atomic_load_explicit(&q->write_index, memory_order_relaxed);
    atomic_store_explicit(&q->write_index, current_write + 1, memory_order_release);
}

/// Consumer (Called by the native libav encoder thread)
/**
 * @brief Pops an audio frame from the lock-free queue.
 * @param q Pointer to the queue structure.
 * @param frame Pointer to store the retrieved audio frame.
 * @return 1 on success, 0 if the buffer is empty.
 */
static inline int pop_audio_frame(SPSC_Audio_Queue* q, AudioFrame* frame) {
    size_t current_read = atomic_load_explicit(&q->read_index, memory_order_relaxed);
    size_t current_write = atomic_load_explicit(&q->write_index, memory_order_acquire);

    if (current_write == current_read) {
        return 0; // Buffer empty
    }

    *frame = q->buffer[current_read & (q->capacity - 1)];
    atomic_store_explicit(&q->read_index, current_read + 1, memory_order_release);
    return 1;
}

typedef struct {
    float l;
    float r;
} StereoFrame;

typedef struct {
    StereoFrame* buffer;
    size_t capacity;
    alignas(CACHE_LINE_SIZE) atomic_size_t write_index;
    alignas(CACHE_LINE_SIZE) atomic_size_t read_index;
} SPSC_Stereo_Queue;

/**
 * @brief Initializes the lock-free single-producer single-consumer stereo queue.
 * @param q Pointer to the queue structure.
 * @param capacity The maximum number of frames (must be a power of two).
 * @return void
 */
static inline void init_stereo_queue(SPSC_Stereo_Queue* q, size_t capacity) {
    q->buffer = (StereoFrame*)calloc(capacity, sizeof(StereoFrame));
    q->capacity = capacity;
    atomic_init(&q->write_index, 0);
    atomic_init(&q->read_index, 0);
}

/**
 * @brief Acquires a pointer to the next available ringbuffer slot to write data directly without copying.
 * @param q Pointer to the queue structure.
 * @return Pointer to a StereoFrame, or NULL if the buffer is full.
 */
static inline StereoFrame* acquire_stereo_frame(SPSC_Stereo_Queue* q) {
    size_t current_write = atomic_load_explicit(&q->write_index, memory_order_relaxed);
    size_t current_read = atomic_load_explicit(&q->read_index, memory_order_acquire);

    if (current_write - current_read >= q->capacity) return NULL;
    return &q->buffer[current_write & (q->capacity - 1)];
}

/**
 * @brief Commits the previously acquired stereo frame to the queue.
 * @param q Pointer to the queue structure.
 * @return void
 */
static inline void commit_stereo_frame(SPSC_Stereo_Queue* q) {
    size_t current_write = atomic_load_explicit(&q->write_index, memory_order_relaxed);
    atomic_store_explicit(&q->write_index, current_write + 1, memory_order_release);
}

/**
 * @brief Pops a stereo frame from the lock-free queue.
 * @param q Pointer to the queue structure.
 * @param frame Pointer to store the retrieved stereo frame.
 * @return 1 on success, 0 if the buffer is empty.
 */
static inline int pop_stereo_frame(SPSC_Stereo_Queue* q, StereoFrame* frame) {
    size_t current_read = atomic_load_explicit(&q->read_index, memory_order_relaxed);
    size_t current_write = atomic_load_explicit(&q->write_index, memory_order_acquire);

    if (current_write == current_read) return 0;

    *frame = q->buffer[current_read & (q->capacity - 1)];
    atomic_store_explicit(&q->read_index, current_read + 1, memory_order_release);
    return 1;
}

/**
 * @brief Lightweight command payload safely passed to the RT thread.
 */
typedef struct {
    EngineCommandType type;
    int target_track; ///< Optional index for commands targeting specific layers
} EngineCommand;

/**
 * @brief Lock-free Single-Producer Single-Consumer queue for UI to RT engine commands.
 */
typedef struct {
    EngineCommand* buffer;
    size_t capacity; ///< Must be a power of 2
    alignas(CACHE_LINE_SIZE) atomic_size_t write_index;
    alignas(CACHE_LINE_SIZE) atomic_size_t read_index;
} SPSC_Command_Queue;

/**
 * @brief Initializes the lock-free command queue.
 * @param q Pointer to the command queue structure.
 * @param capacity The maximum number of commands (must be a power of two).
 * @return void
 */
static inline void init_command_queue(SPSC_Command_Queue* q, size_t capacity) {
    q->buffer = (EngineCommand*)calloc(capacity, sizeof(EngineCommand));
    q->capacity = capacity;
    atomic_init(&q->write_index, 0);
    atomic_init(&q->read_index, 0);
}

/**
 * @brief Pushes a command from the GTK UI thread to the RT audio thread.
 * @param q Pointer to the command queue.
 * @param cmd The command payload to dispatch.
 * @return 1 on success, 0 if the queue is full.
 */
static inline int push_command(SPSC_Command_Queue* q, EngineCommand cmd) {
    size_t current_write = atomic_load_explicit(&q->write_index, memory_order_relaxed);
    size_t current_read = atomic_load_explicit(&q->read_index, memory_order_acquire);

    if (current_write - current_read >= q->capacity) return 0;

    q->buffer[current_write & (q->capacity - 1)] = cmd;
    atomic_store_explicit(&q->write_index, current_write + 1, memory_order_release);
    return 1;
}

/**
 * @brief Pops a command from the queue inside the JACK RT thread.
 * @param q Pointer to the command queue.
 * @param out_cmd Pointer to store the retrieved command.
 * @return 1 on success, 0 if the queue is empty.
 */
static inline int pop_command(SPSC_Command_Queue* q, EngineCommand* out_cmd) {
    size_t current_read = atomic_load_explicit(&q->read_index, memory_order_relaxed);
    size_t current_write = atomic_load_explicit(&q->write_index, memory_order_acquire);

    if (current_write == current_read) return 0;

    *out_cmd = q->buffer[current_read & (q->capacity - 1)];
    atomic_store_explicit(&q->read_index, current_read + 1, memory_order_release);
    return 1;
}

#endif // QJAMS_RINGBUFFER_H
