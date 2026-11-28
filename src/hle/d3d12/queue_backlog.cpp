#include "hle/d3d12/queue_backlog.h"

#include <stdlib.h>
#include <string.h>

BacklogOp* backlog_push(QueueBacklog* b) {
    if (b->count == b->capacity) {
        u32 capacity = b->capacity ? b->capacity * 2 : 16;
        BacklogOp* grown = (BacklogOp*)malloc(capacity * sizeof(BacklogOp));
        if (!grown)
            return nullptr;
        // Unwrapped into the new storage: oldest first from 0.
        for (u32 i = 0; i < b->count; i++)
            grown[i] = b->ops[(b->first + i) % b->capacity];
        free(b->ops);
        b->ops = grown;
        b->first = 0;
        b->capacity = capacity;
    }
    BacklogOp* op = &b->ops[(b->first + b->count) % b->capacity];
    memset(op, 0, sizeof(*op));
    b->count++;
    return op;
}

void backlog_pop(QueueBacklog* b) {
    if (!b->count)
        return;
    b->first = (b->first + 1) % b->capacity;
    b->count--;
}

void backlog_free(QueueBacklog* b) {
    free(b->ops);
    b->ops = nullptr;
    b->first = b->count = b->capacity = 0;
}
