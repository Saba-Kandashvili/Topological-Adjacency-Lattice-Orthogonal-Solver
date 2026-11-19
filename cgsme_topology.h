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

// pack collapsed bitmasks in-place to tile indices (visited bit cleared)
void findConnectedRegionsInPlace(cgsme_tile_t **grid, uint32_t width, uint32_t length);

// 1-bit ariadne welder for disconnected components
void germanWelderInPlace(cgsme_tile_t **grid, uint32_t width, uint32_t length, uint32_t *rng);

void germanWelderExInPlace(
    cgsme_tile_t **grid, uint32_t width, uint32_t length, uint32_t *rng,
    bool *outAllConnected, bool *outUsedSuperset);

void sealMazeEdges(cgsme_tile_t **gridLayer, uint32_t width, uint32_t length);
void fixupEdges(cgsme_tile_t **gridLayer, uint32_t width, uint32_t length);
void openWallPacked(cgsme_tile_t **grid, uint32_t x, uint32_t y, uint8_t dirIdx);
bool repairMazePortsInPlace(cgsme_tile_t **gridLayer, uint32_t width, uint32_t length);
void pruneUnreachableComponentsInPlace(cgsme_tile_t **gridLayer, uint32_t width, uint32_t length);

#endif // CGSME_TOPOLOGY_H
