#ifndef QJAMS_VIDEO_RINGBUFFER_H
#define QJAMS_VIDEO_RINGBUFFER_H
#include <stdalign.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#define CACHE_LINE_SIZE 64
typedef struct {
    uint8_t *data;
    size_t size;
    int64_t pts;
} VideoPayload;
typedef struct {
    VideoPayload *buffer;
    size_t capacity;
    alignas(CACHE_LINE_SIZE) atomic_size_t write_index;
    alignas(CACHE_LINE_SIZE) atomic_size_t read_index;
} SPSC_Video_Queue;

/**
 * @brief Initializes the video capture ringbuffer queue.
 * @param q Pointer to the video queue.
 * @param capacity Power-of-two capacity.
 */
static inline void init_video_queue(SPSC_Video_Queue *q, size_t capacity) {
    q->buffer = (VideoPayload *)calloc(capacity, sizeof(VideoPayload));
    q->capacity = capacity;
    atomic_init(&q->write_index, 0);
    atomic_init(&q->read_index, 0);
}

/**
 * @brief Pushes a raw NV12 frame into the encoder queue.
 * @param q Pointer to the video queue.
 * @param data Pointer to the NV12 buffer.
 * @param size Buffer byte size.
 * @param pts Presentation timestamp in microseconds.
 * @return 1 on success, 0 if full.
 */
static inline int push_video_frame(SPSC_Video_Queue *q, uint8_t *data, size_t size, int64_t pts) {
    size_t current_write = atomic_load_explicit(&q->write_index, memory_order_relaxed);
    size_t current_read = atomic_load_explicit(&q->read_index, memory_order_acquire);
    if (current_write - current_read >= q->capacity) {
        return 0;
    }
    size_t mask = q->capacity - 1;
    q->buffer[current_write & mask].data = data;
    q->buffer[current_write & mask].size = size;
    q->buffer[current_write & mask].pts = pts;
    atomic_store_explicit(&q->write_index, current_write + 1, memory_order_release);
    return 1;
}

/**
 * @brief Pops a raw NV12 frame from the encoder queue.
 * @param q Pointer to the video queue.
 * @param out_payload Output struct receiving the frame metadata and pointer.
 * @return 1 on success, 0 if empty.
 */
static inline int pop_video_frame(SPSC_Video_Queue *q, VideoPayload *out_payload) {
    size_t current_read = atomic_load_explicit(&q->read_index, memory_order_relaxed);
    size_t current_write = atomic_load_explicit(&q->write_index, memory_order_acquire);
    if (current_write == current_read) {
        return 0;
    }
    size_t mask = q->capacity - 1;
    *out_payload = q->buffer[current_read & mask];
    atomic_store_explicit(&q->read_index, current_read + 1, memory_order_release);
    return 1;
}
typedef struct {
    uint8_t *rgba_data;
    int width;
    int height;
} PreviewPayload;
typedef struct {
    PreviewPayload *buffer;
    size_t capacity;
    alignas(CACHE_LINE_SIZE) atomic_size_t write_index;
    alignas(CACHE_LINE_SIZE) atomic_size_t read_index;
    alignas(CACHE_LINE_SIZE) atomic_flag producer_lock;
} SPSC_Preview_Queue;

/**
 * @brief Initializes the video preview ringbuffer queue and producer spinlock.
 * @param q Pointer to the preview queue.
 * @param capacity Power-of-two capacity.
 */
static inline void init_preview_queue(SPSC_Preview_Queue *q, size_t capacity) {
    q->buffer = (PreviewPayload *)calloc(capacity, sizeof(PreviewPayload));
    q->capacity = capacity;
    atomic_init(&q->write_index, 0);
    atomic_init(&q->read_index, 0);
    atomic_flag_clear(&q->producer_lock);
}

/**
 * @brief Thread-safe multi-producer push into the video preview queue.
 * @param q Pointer to the preview queue.
 * @param rgba_data Pointer to the RGBA pixel buffer.
 * @param width Frame width in pixels.
 * @param height Frame height in pixels.
 * @return 1 on success, 0 if full.
 */
static inline int push_preview_frame(SPSC_Preview_Queue *q, uint8_t *rgba_data, int width,
                                     int height) {
    while (atomic_flag_test_and_set_explicit(&q->producer_lock, memory_order_acquire)) {
    }
    size_t current_write = atomic_load_explicit(&q->write_index, memory_order_relaxed);
    size_t current_read = atomic_load_explicit(&q->read_index, memory_order_acquire);
    if (current_write - current_read >= q->capacity) {
        atomic_flag_clear_explicit(&q->producer_lock, memory_order_release);
        return 0;
    }
    size_t mask = q->capacity - 1;
    q->buffer[current_write & mask].rgba_data = rgba_data;
    q->buffer[current_write & mask].width = width;
    q->buffer[current_write & mask].height = height;
    atomic_store_explicit(&q->write_index, current_write + 1, memory_order_release);
    atomic_flag_clear_explicit(&q->producer_lock, memory_order_release);
    return 1;
}

/**
 * @brief Pops an RGBA frame from the preview queue for GTK rendering.
 * @param q Pointer to the preview queue.
 * @param out_payload Output struct receiving the frame dimensions and pointer.
 * @return 1 on success, 0 if empty.
 */
static inline int pop_preview_frame(SPSC_Preview_Queue *q, PreviewPayload *out_payload) {
    size_t current_read = atomic_load_explicit(&q->read_index, memory_order_relaxed);
    size_t current_write = atomic_load_explicit(&q->write_index, memory_order_acquire);
    if (current_write == current_read)
        return 0;
    size_t mask = q->capacity - 1;
    *out_payload = q->buffer[current_read & mask];
    atomic_store_explicit(&q->read_index, current_read + 1, memory_order_release);
    return 1;
}
#endif
