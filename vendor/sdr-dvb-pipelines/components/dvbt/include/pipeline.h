#ifndef RBDVBT_PIPELINE_H
#define RBDVBT_PIPELINE_H

#include <stddef.h>
#include <stdint.h>

#include <pthread.h>

#define RBDVBT_TS_PACKET_SIZE 188u

typedef enum {
    RBDVBT_PIPELINE_EVENT_CONFIG_CHANGED,
    RBDVBT_PIPELINE_EVENT_SYNC_SEARCHING,
    RBDVBT_PIPELINE_EVENT_SYNC_LOCKED,
    RBDVBT_PIPELINE_EVENT_SYNC_HOLDOVER,
    RBDVBT_PIPELINE_EVENT_SYNC_LOST,
    RBDVBT_PIPELINE_EVENT_GENERATION_CHANGED,
    RBDVBT_PIPELINE_EVENT_PIPELINE_FLUSH,
    RBDVBT_PIPELINE_EVENT_FIFO_LEVEL,
    RBDVBT_PIPELINE_EVENT_FIFO_OVERRUN,
    RBDVBT_PIPELINE_EVENT_FIFO_UNDERRUN,
    RBDVBT_PIPELINE_EVENT_DROPOUT,
    RBDVBT_PIPELINE_EVENT_ERROR_RATE_UPDATE
} rbdvbt_pipeline_event_type_t;

typedef struct {
    rbdvbt_pipeline_event_type_t type;
    const char *source_block;
    const char *fifo_name;
    uint32_t generation_id;
    uint64_t sample_index;
    uint64_t item_index;
    uint32_t value;
} rbdvbt_pipeline_event_t;

typedef struct {
    uint32_t generation_id;
    uint32_t tps_config;
    uint64_t first_sample_index;
    uint64_t first_symbol_index;
    uint32_t symbol_count;
    const int16_t *soft_bits;
    size_t soft_bit_count;
    uint32_t quality_flags;
} rbdvbt_fifo1_item_t;

typedef struct {
    uint32_t generation_id;
    uint64_t byte_offset;
    const uint8_t *bytes;
    size_t byte_count;
    double viterbi_metric;
    double ber_estimate;
    uint32_t flags;
} rbdvbt_fifo2_item_t;

typedef struct {
    uint32_t generation_id;
    uint64_t packet_index;
    uint8_t bytes[RBDVBT_TS_PACKET_SIZE];
    uint32_t rs_corrected_errors;
    uint32_t flags;
} rbdvbt_ts_packet_t;

typedef struct {
    const char *sink_name;
    uint32_t generation_id;
    uint64_t packets_written;
    uint32_t flags;
} rbdvbt_fifo4_item_t;

typedef struct {
    const char *name;
    size_t item_size;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
    int closed;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    uint8_t *items;
} rbdvbt_fifo_t;

const char *rbdvbt_pipeline_event_name(rbdvbt_pipeline_event_type_t type);
void rbdvbt_pipeline_publish_event(const rbdvbt_pipeline_event_t *event);

int rbdvbt_fifo_init(rbdvbt_fifo_t *fifo,
                     const char *name,
                     size_t item_size,
                     size_t capacity);
void rbdvbt_fifo_free(rbdvbt_fifo_t *fifo);
int rbdvbt_fifo_try_push(rbdvbt_fifo_t *fifo, const void *item);
int rbdvbt_fifo_try_pop(rbdvbt_fifo_t *fifo, void *item);
int rbdvbt_fifo_push_wait(rbdvbt_fifo_t *fifo, const void *item);
int rbdvbt_fifo_pop_wait(rbdvbt_fifo_t *fifo, void *item);
void rbdvbt_fifo_close(rbdvbt_fifo_t *fifo);
size_t rbdvbt_fifo_count(const rbdvbt_fifo_t *fifo);
size_t rbdvbt_fifo_capacity(const rbdvbt_fifo_t *fifo);
void rbdvbt_fifo_clear(rbdvbt_fifo_t *fifo);

#endif
