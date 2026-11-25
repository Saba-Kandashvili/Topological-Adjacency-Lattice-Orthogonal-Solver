#include "talos_utils.h"
#include "talos_debug.h"
#include "talos_solver.h"
#include <stdio.h>
#include <math.h>

Queue2D *q_init(int cap)
{
	Queue2D *q = malloc(sizeof(Queue2D));
	q->data = malloc(sizeof(Point2D) * cap);
	q->head = q->tail = 0;
	q->cap = cap;
	return q;
}
void q_push(Queue2D *q, int x, int y)
{
	if (q->tail < q->cap)
	{
		q->data[q->tail].x = x;
		q->data[q->tail].y = y;
		q->tail++;
	}
}
int q_pop(Queue2D *q, Point2D *out)
{
	if (q->head >= q->tail)
		return 0;
	*out = q->data[q->head++];
	return 1;
}
void q_free(Queue2D *q)
{
	free(q->data);
	free(q);
}

MinHeap *initHeap(uint32_t width, uint32_t length)
{
	TALOS_PROFILE_FUNC();
	MinHeap *h = malloc(sizeof(MinHeap));
	h->capacity = width * length; // Worst case: everything added
	h->count = 0;
	h->width = width;
	h->length = length;
	h->nodes = malloc(sizeof(HeapNode) * h->capacity);

	// Initialize map to -1 (not in heap)
	h->indexMap = malloc(sizeof(int32_t) * width * length);
	for (uint32_t i = 0; i < width * length; i++)
		h->indexMap[i] = -1;

	return h;
}

void freeHeap(MinHeap *h)
{
	TALOS_PROFILE_FUNC();
	free(h->nodes);
	free(h->indexMap);
	free(h);
}

void swapNodes(MinHeap *h, uint32_t i, uint32_t j)
{
	TALOS_PROFILE_FUNC();
	HeapNode temp = h->nodes[i];
	h->nodes[i] = h->nodes[j];
	h->nodes[j] = temp;

	// update lookup map
	uint32_t idx_i = h->nodes[i].y * h->width + h->nodes[i].x;
	uint32_t idx_j = h->nodes[j].y * h->width + h->nodes[j].x;
	h->indexMap[idx_i] = i;
	h->indexMap[idx_j] = j;
}

void bubbleUp(MinHeap *h, uint32_t index)
{
	TALOS_PROFILE_FUNC();
	while (index > 0)
	{
		uint32_t parent = (index - 1) / 2;
		if (h->nodes[index].score < h->nodes[parent].score)
		{
			swapNodes(h, index, parent);
			index = parent;
		}
		else
		{
			break;
		}
	}
}

void bubbleDown(MinHeap *h, uint32_t index)
{
	TALOS_PROFILE_FUNC();
	while (true)
	{
		uint32_t left = 2 * index + 1;
		uint32_t right = 2 * index + 2;
		uint32_t smallest = index;

		if (left < h->count && h->nodes[left].score < h->nodes[smallest].score)
			smallest = left;
		if (right < h->count && h->nodes[right].score < h->nodes[smallest].score)
			smallest = right;

		if (smallest != index)
		{
			swapNodes(h, index, smallest);
			index = smallest;
		}
		else
		{
			break;
		}
	}
}

// Adds a node or Updates it if it already exists
void heapInsertOrUpdate(MinHeap *h, talos_tile_t **grid, uint32_t x, uint32_t y, uint32_t *rng)
{
	TALOS_PROFILE_FUNC();
	uint32_t mapIdx = y * h->width + x;
	int32_t currentHeapIdx = h->indexMap[mapIdx];

	// Check validity
	if (x >= h->width || y >= h->length)
		return;
	// don't add collapsed tiles (1 bit) or broken tiles (0 bits)
	if (TALOS_POPCOUNT(grid[y][x]) <= 1)
	{
		// IF it was in heap, remove it (lazy removal happens on pop usually, but we can do logic here if strictly needed)
		// for WFC, typically once collapsed we just ignore it.
		// IF it IS in the heap (rare race condition logic), we leave it, it will be popped and ignored.
		return;
	}

	// Calculate fresh score
	float score = calculateScore(grid, x, y, rng);

	if (currentHeapIdx != -1)
	{
		// already in heap, update score and rebalance
		if (score < h->nodes[currentHeapIdx].score)
		{
			h->nodes[currentHeapIdx].score = score;
			bubbleUp(h, currentHeapIdx);
		}
		else if (score > h->nodes[currentHeapIdx].score)
		{
			h->nodes[currentHeapIdx].score = score;
			bubbleDown(h, currentHeapIdx);
		}
	}
	else
	{
		// NEW INSERTION
		uint32_t idx = h->count;
		h->nodes[idx].x = x;
		h->nodes[idx].y = y;
		h->nodes[idx].score = score;
		h->indexMap[mapIdx] = idx;
		h->count++;
		bubbleUp(h, idx);
	}
}

// returns true if valid node found, false if empty
bool heapPop(MinHeap *h, talos_tile_t **grid, uint32_t *outX, uint32_t *outY)
{
	TALOS_PROFILE_FUNC();
	while (h->count > 0)
	{
		// take top
		HeapNode top = h->nodes[0];

		// remove top (swap with last, decrease count)
		uint32_t lastIdx = h->count - 1;
		swapNodes(h, 0, lastIdx);
		h->count--;
		h->indexMap[top.y * h->width + top.x] = -1; // Mark as removed

		if (h->count > 0)
			bubbleDown(h, 0);

		// unlikely in single thread, but good safety to check if its been added to the heap adn then later collapsed by something else. (or if it was added to heap, then later collapsed by a neighbor update)
		if (TALOS_POPCOUNT(grid[top.y][top.x]) > 1)		
		{
			*outX = top.x;
			*outY = top.y;
			return true;
		}
		// IF not valid loop again (lazy deletion)
	}
	return false;
}