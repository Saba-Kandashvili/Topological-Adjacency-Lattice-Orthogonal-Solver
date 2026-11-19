#ifndef CGSME_SOLVER_H
#define CGSME_SOLVER_H
#include <stdint.h>
#include <stdbool.h>
#include "threadRandom.h"
#include "cgsme_utils.h"
#include "tiles.h"

// gauss config (CGSME_GAUSS_WIDTH, CGSME_CONNECTOR_BOOST) and the category
// tables (CGSME_BIT_TO_CATEGORY, CGSME_CATEGORY_CURVE_POS, ...) now live in
// cgsme_tileset_baked.h and are generated from tileset.json

// collapse tile bitmask to a single variant using category weights
void collapseTile(cgsme_tile_t *tile, float *rates, uint32_t *rng);

// propagate constraints from (x,y) to its neighbors via CGSME_PROP_MASK
void updateNeighbours(cgsme_tile_t **gridLayer, uint32_t width, uint32_t length, uint32_t x, uint32_t y, MinHeap *heap, uint32_t *rng);

// update category spawn weights along the gaussian curve
void update_spawnrates(float rates[], int current_collapsed, int target_collapsed);

bool isTileRequired(cgsme_tile_t **grid, uint32_t width, uint32_t length, uint32_t x, uint32_t y);
bool findBestSeedLocation(cgsme_tile_t **grid, uint32_t width, uint32_t length, uint32_t *outX, uint32_t *outY, uint32_t *rng);
float calculateScore(cgsme_tile_t **grid, uint32_t x, uint32_t y, uint32_t *rng);
cgsme_tile_t exactCellValidMask(cgsme_tile_t **gridLayer, uint32_t width, uint32_t length, uint32_t x, uint32_t y);
void backtrackNeighborhood(cgsme_tile_t **gridLayer, uint32_t width, uint32_t length, uint32_t cx, uint32_t cy, MinHeap *heap, uint32_t *rng);

typedef struct
{
    uint16_t lx;
    uint16_t ly;
    uint16_t z;
    float score;
} ChunkHeapNode3D;

typedef struct
{
    ChunkHeapNode3D *nodes;
    int32_t *indexMap;
    uint32_t count;
    uint32_t capacity;
    uint32_t x0, y0;
    uint32_t cw, cl, height;
} ChunkHeap3D;

ChunkHeap3D *initChunkHeap3D(uint32_t max_cw, uint32_t max_cl, uint32_t height);
void freeChunkHeap3D(ChunkHeap3D *h);
void bindChunkHeap3D(ChunkHeap3D *h, uint32_t x0, uint32_t y0, uint32_t cw, uint32_t cl, uint32_t height);
void chunkHeapInsertOrUpdate3D(ChunkHeap3D *h, cgsme_tile_t ***grid, uint32_t x, uint32_t y, uint32_t z, uint32_t *rng);
bool chunkHeapPop3D(ChunkHeap3D *h, cgsme_tile_t ***grid, uint32_t *outX, uint32_t *outY, uint32_t *outZ);

cgsme_tile_t exactCellValidMask3D(cgsme_tile_t ***grid, uint32_t width, uint32_t length, uint32_t height, uint32_t x, uint32_t y, uint32_t z);
void updateNeighboursChunk3D(cgsme_tile_t ***grid, uint32_t width, uint32_t length, uint32_t height, ChunkHeap3D *heap, uint32_t x, uint32_t y, uint32_t z, uint32_t *rng);
void backtrackChunkNeighborhood3D(cgsme_tile_t ***grid, uint32_t width, uint32_t length, uint32_t height, ChunkHeap3D *heap, uint32_t cx, uint32_t cy, uint32_t cz, uint32_t *rng);
bool findBestSeedLocationChunk3D(cgsme_tile_t ***grid, uint32_t width, uint32_t length, uint32_t height, ChunkHeap3D *heap, uint32_t *outX, uint32_t *outY, uint32_t *outZ, uint32_t *rng);

#endif // CGSME_SOLVER_H
