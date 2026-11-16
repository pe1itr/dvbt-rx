#include "pipeline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *rbdvbt_pipeline_event_name(rbdvbt_pipeline_event_type_t type)
{
    switch (type) {
    case RBDVBT_PIPELINE_EVENT_CONFIG_CHANGED:
        return "CONFIG_CHANGED";
    case RBDVBT_PIPELINE_EVENT_SYNC_SEARCHING:
        return "SYNC_SEARCHING";
    case RBDVBT_PIPELINE_EVENT_SYNC_LOCKED:
        return "SYNC_LOCKED";
    case RBDVBT_PIPELINE_EVENT_SYNC_HOLDOVER:
        return "SYNC_HOLDOVER";
    case RBDVBT_PIPELINE_EVENT_SYNC_LOST:
        return "SYNC_LOST";
    case RBDVBT_PIPELINE_EVENT_GENERATION_CHANGED:
        return "GENERATION_CHANGED";
    case RBDVBT_PIPELINE_EVENT_PIPELINE_FLUSH:
        return "PIPELINE_FLUSH";
    case RBDVBT_PIPELINE_EVENT_FIFO_LEVEL:
        return "FIFO_LEVEL";
    case RBDVBT_PIPELINE_EVENT_FIFO_OVERRUN:
        return "FIFO_OVERRUN";
    case RBDVBT_PIPELINE_EVENT_FIFO_UNDERRUN:
        return "FIFO_UNDERRUN";
    case RBDVBT_PIPELINE_EVENT_DROPOUT:
        return "DROPOUT";
    case RBDVBT_PIPELINE_EVENT_ERROR_RATE_UPDATE:
        return "ERROR_RATE_UPDATE";
    default:
        return "UNKNOWN";
    }
}

void rbdvbt_pipeline_publish_event(const rbdvbt_pipeline_event_t *event)
{
    if (event == NULL) {
        return;
    }

    fprintf(stderr,
            "[pipeline] event=%s source=%s fifo=%s generation=%u sample=%llu item=%llu value=%u\n",
            rbdvbt_pipeline_event_name(event->type),
            event->source_block != NULL ? event->source_block : "-",
            event->fifo_name != NULL ? event->fifo_name : "-",
            event->generation_id,
            (unsigned long long)event->sample_index,
            (unsigned long long)event->item_index,
            event->value);
}

int rbdvbt_fifo_init(rbdvbt_fifo_t *fifo,
                     const char *name,
                     size_t item_size,
                     size_t capacity)
{
    if (fifo == NULL || item_size == 0u || capacity == 0u) {
        return -1;
    }

    memset(fifo, 0, sizeof(*fifo));
    if (pthread_mutex_init(&fifo->mutex, NULL) != 0) {
        return -1;
    }
    if (pthread_cond_init(&fifo->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&fifo->mutex);
        return -1;
    }
    if (pthread_cond_init(&fifo->not_full, NULL) != 0) {
        pthread_cond_destroy(&fifo->not_empty);
        pthread_mutex_destroy(&fifo->mutex);
        return -1;
    }

    fifo->items = calloc(capacity, item_size);
    if (fifo->items == NULL) {
        pthread_cond_destroy(&fifo->not_full);
        pthread_cond_destroy(&fifo->not_empty);
        pthread_mutex_destroy(&fifo->mutex);
        return -1;
    }

    fifo->name = name;
    fifo->item_size = item_size;
    fifo->capacity = capacity;
    return 0;
}

void rbdvbt_fifo_free(rbdvbt_fifo_t *fifo)
{
    if (fifo == NULL) {
        return;
    }

    free(fifo->items);
    pthread_cond_destroy(&fifo->not_full);
    pthread_cond_destroy(&fifo->not_empty);
    pthread_mutex_destroy(&fifo->mutex);
    memset(fifo, 0, sizeof(*fifo));
}

static void fifo_push_locked(rbdvbt_fifo_t *fifo, const void *item)
{
    uint8_t *slot;

    slot = fifo->items + fifo->tail * fifo->item_size;
    memcpy(slot, item, fifo->item_size);
    fifo->tail = (fifo->tail + 1u) % fifo->capacity;
    fifo->count++;
}

static void fifo_pop_locked(rbdvbt_fifo_t *fifo, void *item)
{
    const uint8_t *slot;

    slot = fifo->items + fifo->head * fifo->item_size;
    memcpy(item, slot, fifo->item_size);
    fifo->head = (fifo->head + 1u) % fifo->capacity;
    fifo->count--;
}

int rbdvbt_fifo_try_push(rbdvbt_fifo_t *fifo, const void *item)
{
    int rc = 0;

    if (fifo == NULL || item == NULL || fifo->items == NULL) {
        return -1;
    }
    if (pthread_mutex_lock(&fifo->mutex) != 0) {
        return -1;
    }
    if (fifo->closed) {
        rc = -1;
        goto done;
    }
    if (fifo->count == fifo->capacity) {
        rc = 1;
        goto done;
    }

    fifo_push_locked(fifo, item);
    pthread_cond_signal(&fifo->not_empty);

done:
    pthread_mutex_unlock(&fifo->mutex);
    return rc;
}

int rbdvbt_fifo_try_pop(rbdvbt_fifo_t *fifo, void *item)
{
    int rc = 0;

    if (fifo == NULL || item == NULL || fifo->items == NULL) {
        return -1;
    }
    if (pthread_mutex_lock(&fifo->mutex) != 0) {
        return -1;
    }
    if (fifo->count == 0u) {
        rc = 1;
        goto done;
    }

    fifo_pop_locked(fifo, item);
    pthread_cond_signal(&fifo->not_full);

done:
    pthread_mutex_unlock(&fifo->mutex);
    return rc;
}

int rbdvbt_fifo_push_wait(rbdvbt_fifo_t *fifo, const void *item)
{
    int rc = 0;

    if (fifo == NULL || item == NULL || fifo->items == NULL) {
        return -1;
    }
    if (pthread_mutex_lock(&fifo->mutex) != 0) {
        return -1;
    }
    while (!fifo->closed && fifo->count == fifo->capacity) {
        pthread_cond_wait(&fifo->not_full, &fifo->mutex);
    }
    if (fifo->closed) {
        rc = -1;
        goto done;
    }

    fifo_push_locked(fifo, item);
    pthread_cond_signal(&fifo->not_empty);

done:
    pthread_mutex_unlock(&fifo->mutex);
    return rc;
}

int rbdvbt_fifo_pop_wait(rbdvbt_fifo_t *fifo, void *item)
{
    int rc = 0;

    if (fifo == NULL || item == NULL || fifo->items == NULL) {
        return -1;
    }
    if (pthread_mutex_lock(&fifo->mutex) != 0) {
        return -1;
    }
    while (!fifo->closed && fifo->count == 0u) {
        pthread_cond_wait(&fifo->not_empty, &fifo->mutex);
    }
    if (fifo->count == 0u && fifo->closed) {
        rc = 1;
        goto done;
    }

    fifo_pop_locked(fifo, item);
    pthread_cond_signal(&fifo->not_full);

done:
    pthread_mutex_unlock(&fifo->mutex);
    return rc;
}

void rbdvbt_fifo_close(rbdvbt_fifo_t *fifo)
{
    if (fifo == NULL || fifo->items == NULL) {
        return;
    }
    if (pthread_mutex_lock(&fifo->mutex) != 0) {
        return;
    }
    fifo->closed = 1;
    pthread_cond_broadcast(&fifo->not_empty);
    pthread_cond_broadcast(&fifo->not_full);
    pthread_mutex_unlock(&fifo->mutex);
}

size_t rbdvbt_fifo_count(const rbdvbt_fifo_t *fifo)
{
    size_t count;

    if (fifo == NULL) {
        return 0u;
    }
    if (pthread_mutex_lock((pthread_mutex_t *)&fifo->mutex) != 0) {
        return 0u;
    }
    count = fifo->count;
    pthread_mutex_unlock((pthread_mutex_t *)&fifo->mutex);
    return count;
}

size_t rbdvbt_fifo_capacity(const rbdvbt_fifo_t *fifo)
{
    return fifo != NULL ? fifo->capacity : 0u;
}

void rbdvbt_fifo_clear(rbdvbt_fifo_t *fifo)
{
    if (fifo == NULL) {
        return;
    }

    if (pthread_mutex_lock(&fifo->mutex) != 0) {
        return;
    }
    fifo->head = 0u;
    fifo->tail = 0u;
    fifo->count = 0u;
    pthread_cond_broadcast(&fifo->not_full);
    pthread_mutex_unlock(&fifo->mutex);
}
