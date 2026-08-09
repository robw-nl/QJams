#ifndef QJAMS_VIDEO_RINGBUFFER_H
#define QJAMS_VIDEO_RINGBUFFER_H

#include <stdint.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <stdalign.h>

#define CACHE_LINE_SIZE 64

// The payload for a single video frame
typedef struct {
    uint8_t* data;
    size_t size;
    int64_t pts; // Presentation timestamp to sync with audio
} VideoPayload;

typedef struct {
    VideoPayload* buffer;
    size_t capacity; // Must be a power of 2

    alignas(CACHE_LINE_SIZE) atomic_size_t write_index;
    alignas(CACHE_LINE_SIZE) atomic_size_t read_index;
} SPSC_Video_Queue;

/**
 * @brief Initializes the lock-free single-producer single-consumer video queue.
 * @param q Pointer to the video queue structure.
 * @param capacity The maximum number of frames (must be a power of two).
 * @return void
 */
static inline void init_video_queue(SPSC_Video_Queue* q, size_t capacity) {
    q->buffer = (VideoPayload*)calloc(capacity, sizeof(VideoPayload));
    q->capacity = capacity;
    atomic_init(&q->write_index, 0);
    atomic_init(&q->read_index, 0);
}

// Called by the V4L2 capture thread
/**
 * @brief Pushes a new video payload into the lock-free queue.
 * @param q Pointer to the video queue structure.
 * @param data Pointer to the raw frame memory.
 * @param size The size of the frame data in bytes.
 * @param pts The presentation timestamp of the frame.
 * @return 1 on success, 0 if the buffer is full.
 */
static inline int push_video_frame(SPSC_Video_Queue* q, uint8_t* data, size_t size, int64_t pts) {
    size_t current_write = atomic_load_explicit(&q->write_index, memory_order_relaxed);
    size_t current_read = atomic_load_explicit(&q->read_index, memory_order_acquire);

    if (current_write - current_read >= q->capacity) {
        return 0; // Queue full - dropped frame
    }

    size_t mask = q->capacity - 1;
    q->buffer[current_write & mask].data = data;
    q->buffer[current_write & mask].size = size;
    q->buffer[current_write & mask].pts = pts;

    atomic_store_explicit(&q->write_index, current_write + 1, memory_order_release);
    return 1;
}

// Called by the libav encoder thread
/**
 * @brief Pops a video payload from the lock-free queue.
 * @param q Pointer to the video queue structure.
 * @param out_payload Pointer to store the retrieved video payload.
 * @return 1 on success, 0 if the buffer is empty.
 */
static inline int pop_video_frame(SPSC_Video_Queue* q, VideoPayload* out_payload) {
    size_t current_read = atomic_load_explicit(&q->read_index, memory_order_relaxed);
    size_t current_write = atomic_load_explicit(&q->write_index, memory_order_acquire);

    if (current_write == current_read) {
        return 0; // Queue empty
    }

    size_t mask = q->capacity - 1;
    *out_payload = q->buffer[current_read & mask];

    atomic_store_explicit(&q->read_index, current_read + 1, memory_order_release);
    return 1;
}

// --- UI PREVIEW QUEUE ---
typedef struct {
    uint8_t* rgba_data;
    int width;
    int height;
} PreviewPayload;

typedef struct {
    PreviewPayload* buffer;
    size_t capacity;
    alignas(CACHE_LINE_SIZE) atomic_size_t write_index;
    alignas(CACHE_LINE_SIZE) atomic_size_t read_index;
} SPSC_Preview_Queue;

/**
 * @brief Initializes the lock-free queue for UI video previews.
 * @param q Pointer to the preview queue structure.
 * @param capacity The maximum number of frames.
 * @return void
 */
static inline void init_preview_queue(SPSC_Preview_Queue* q, size_t capacity) {
    q->buffer = (PreviewPayload*)calloc(capacity, sizeof(PreviewPayload));
    q->capacity = capacity;
    atomic_init(&q->write_index, 0);
    atomic_init(&q->read_index, 0);
}

/**
 * @brief Pushes a new RGBA preview frame into the queue.
 * @param q Pointer to the preview queue structure.
 * @param rgba_data Pointer to the RGBA frame memory.
 * @param width The width of the preview frame.
 * @param height The height of the preview frame.
 * @return 1 on success, 0 if the buffer is full.
 */
static inline int push_preview_frame(SPSC_Preview_Queue* q, uint8_t* rgba_data, int width, int height) {
    size_t current_write = atomic_load_explicit(&q->write_index, memory_order_relaxed);
    size_t current_read = atomic_load_explicit(&q->read_index, memory_order_acquire);

    if (current_write - current_read >= q->capacity) return 0; // Drop frame

    size_t mask = q->capacity - 1;
    q->buffer[current_write & mask].rgba_data = rgba_data;
    q->buffer[current_write & mask].width = width;
    q->buffer[current_write & mask].height = height;

    atomic_store_explicit(&q->write_index, current_write + 1, memory_order_release);
    return 1;
}

/**
 * @brief Pops an RGBA preview frame from the queue.
 * @param q Pointer to the preview queue structure.
 * @param out_payload Pointer to store the retrieved preview payload.
 * @return 1 on success, 0 if the buffer is empty.
 */
static inline int pop_preview_frame(SPSC_Preview_Queue* q, PreviewPayload* out_payload) {
    size_t current_read = atomic_load_explicit(&q->read_index, memory_order_relaxed);
    size_t current_write = atomic_load_explicit(&q->write_index, memory_order_acquire);

    if (current_write == current_read) return 0; // Empty

    size_t mask = q->capacity - 1;
    *out_payload = q->buffer[current_read & mask];

    atomic_store_explicit(&q->read_index, current_read + 1, memory_order_release);
    return 1;
}

#endif // QJAMS_VIDEO_RINGBUFFER_H
