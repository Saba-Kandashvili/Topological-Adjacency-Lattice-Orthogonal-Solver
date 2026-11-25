#ifndef TALOS_UTILS_H
#define TALOS_UTILS_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include "talos_debug.h"
#include "tiles.h"

// QUEUE FOR FLOOD FILL
typedef struct
{
    int32_t x, y;
} Point2D;
typedef struct
{
    Point2D *data;
    int head, tail, cap;
} Queue2D;

Queue2D *q_init(int cap);
void q_push(Queue2D *q, int x, int y);
int q_pop(Queue2D *q, Point2D *out);
void q_free(Queue2D *q);

// min-heap

typedef struct
{
    uint32_t x;
    uint32_t y;
    float score; // cached score (entropy + noise)
} HeapNode;

typedef struct
{
    HeapNode *nodes;   // binary heap array
    int32_t *indexMap; // lookup table: map[y * width + x] = heap_index (or -1)
    uint32_t count;
    uint32_t capacity;
    uint32_t width; // for index calculation
    uint32_t length;
} MinHeap;

MinHeap *initHeap(uint32_t width, uint32_t length);
void freeHeap(MinHeap *h);
void swapNodes(MinHeap *h, uint32_t i, uint32_t j);
void bubbleUp(MinHeap *h, uint32_t index);
void bubbleDown(MinHeap *h, uint32_t index);
void heapInsertOrUpdate(MinHeap *h, talos_tile_t **grid, uint32_t x, uint32_t y, uint32_t *rng);
bool heapPop(MinHeap *h, talos_tile_t **grid, uint32_t *outX, uint32_t *outY);

#endif // TALOS_UTILS_H