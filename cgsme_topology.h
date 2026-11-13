#ifndef CGSME_TOPOLOGY_H
#define CGSME_TOPOLOGY_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include "tiles.h"

// internal representation of a node in BFS / frontier expansion
typedef struct
{
    uint32_t x, y;
} TopoNode;

// candidate bridge across a closed wall inside the Ariadne frontier
typedef struct
{
    uint32_t x;
    uint32_t y;
    uint8_t dir; // DIR_IDX_N (0), DIR_IDX_E (1), DIR_IDX_S (2), DIR_IDX_W (3)
} AriadneEdge;

/// @brief Compress collapsed tile bitmasks in-place to compact indices [0 .. CGSME_NUM_TILES-1],
///        with CGSME_VISITED_BIT initially cleared and void cells set to CGSME_PACKED_VOID.
void findConnectedRegionsInPlace(cgsme_tile_t **grid, uint32_t width, uint32_t length);

/// @brief Weld all adjacent disconnected components in-place using the 1-bit "Ariadne"
///        quotient-graph Randomized Prim welder (zero region-ID limit, works for 8/16/32/64-bit grids).
void germanWelderInPlace(cgsme_tile_t **grid, uint32_t width, uint32_t length, uint32_t *rng);

/// @brief Seal maze edges by filling void tiles adjacent to open corridor ports.
void sealMazeEdges(cgsme_tile_t **gridLayer, uint32_t width, uint32_t length);

/// @brief Fix edges at grid boundaries by closing out-of-bounds ports via CGSME_CLOSE_LUT.
void fixupEdges(cgsme_tile_t **gridLayer, uint32_t width, uint32_t length);

/// @brief Open port dirIdx (0..3) on packed cell (x,y) via CGSME_OPEN_LUT, preserving CGSME_VISITED_BIT.
void openWallPacked(cgsme_tile_t **grid, uint32_t x, uint32_t y, uint8_t dirIdx);

#endif // CGSME_TOPOLOGY_H
