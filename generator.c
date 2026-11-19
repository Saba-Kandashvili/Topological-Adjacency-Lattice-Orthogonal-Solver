#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <stdbool.h>
#include <stdio.h>
#if !defined(__STDC_NO_ATOMICS__) && !defined(_MSC_VER)
#include <stdatomic.h>
#define CGSME_HAS_C11_ATOMICS 1
#endif
#ifndef _MSC_VER
#include <sys/param.h>
#endif
#include <math.h>

#include "threadRandom.h"
#include "generator.h"
#include "cgsme_debug.h"
#include "tiles.h"
#include "tinycthread/tinycthread.h"
#include "cgsme_utils.h"
#include "cgsme_noise.h"
#include "cgsme_topology.h"
#include "cgsme_solver.h"

#ifndef __linux__
#define MAX(a, b) ((a) > (b) ? a : b)
#define MIN(a, b) ((a) < (b) ? a : b)
#endif

uint32_t w, l;

/// generate a 3D grid using wfc
///
/// parameters:
///     width - number of columns per layer
///     length - number of rows per layer
///     height - number of layers
///     seed - RNG seed used to initialize per thread RNGs
///     fulness - target percentage (0-100) of tiles to collapse; a minimum collapsed count is enforced by the implementation
///
/// returns:
///     pointer to a newly allocated 3D grid. The return value is an array of `height` pointers to layers; each layer is an array of `length` row
///     pointers and each row contains `width` `cgsme_tile_t` entries. The caller owns the returned memory and must free it with `freeGrid(grid, width, length, height)`
///

cgsme_tile_t ***generateGrid(uint32_t width, uint32_t length, uint32_t height, uint32_t seed, uint32_t fulness);

// frees grid from generateGrid
void freeGrid(cgsme_tile_t ***grid, uint32_t width, uint32_t length, uint32_t height);
// worker thread for single layer
int generateLayerThread(void *args);


void printProgressBar(int percentage)
{
    CGSME_PROFILE_FUNC(); // bugg
    int barWidth = 70;    // Width of the progress bar
    printf("[");
    int pos = barWidth * percentage / 100;
    for (int i = 0; i < barWidth; ++i)
    {
        if (i < pos)
            printf("=");
        else if (i == pos)
            printf(">");
        else
            printf(" ");
    }

    printf("] %d %%\r", percentage);
    fflush(stdout);
}

// EDGE FIXUP LOGIC


static inline cgsme_tile_t cgsmeHasAnyOpenPort(void)
{
    return CGSME_HAS_PORT_MASK[DIR_IDX_N] | CGSME_HAS_PORT_MASK[DIR_IDX_E] |
           CGSME_HAS_PORT_MASK[DIR_IDX_S] | CGSME_HAS_PORT_MASK[DIR_IDX_W];
}

static inline bool cgsmeHasVoidRestrictions(void)
{
    return (CGSME_PROP_MASK[CGSME_NUM_TILES][DIR_IDX_N] != All_Possible_State) ||
           (CGSME_PROP_MASK[CGSME_NUM_TILES][DIR_IDX_E] != All_Possible_State) ||
           (CGSME_PROP_MASK[CGSME_NUM_TILES][DIR_IDX_S] != All_Possible_State) ||
           (CGSME_PROP_MASK[CGSME_NUM_TILES][DIR_IDX_W] != All_Possible_State);
}

static inline uint8_t layerVoidOobMask4(cgsme_tile_t **layer, uint32_t width, uint32_t length, uint32_t x, uint32_t y)
{
    uint8_t voidDirs = 0;
    if (y == 0 || layer[y - 1][x] == Empty_Tile)
        voidDirs |= (1u << DIR_IDX_N);
    if (x + 1 >= width || layer[y][x + 1] == Empty_Tile)
        voidDirs |= (1u << DIR_IDX_E);
    if (y + 1 >= length || layer[y + 1][x] == Empty_Tile)
        voidDirs |= (1u << DIR_IDX_S);
    if (x == 0 || layer[y][x - 1] == Empty_Tile)
        voidDirs |= (1u << DIR_IDX_W);
    return voidDirs;
}

static bool tileFitsVoidAndOob(cgsme_tile_t **layer, uint32_t width, uint32_t length, uint32_t x, uint32_t y, cgsme_tile_t tileMask)
{
    if ((y == 0 || layer[y - 1][x] == Empty_Tile) && !(tileMask & CGSME_PROP_MASK[CGSME_NUM_TILES][DIR_IDX_S]))
        return false;
    if ((x + 1 >= width || layer[y][x + 1] == Empty_Tile) && !(tileMask & CGSME_PROP_MASK[CGSME_NUM_TILES][DIR_IDX_W]))
        return false;
    if ((y + 1 >= length || layer[y + 1][x] == Empty_Tile) && !(tileMask & CGSME_PROP_MASK[CGSME_NUM_TILES][DIR_IDX_N]))
        return false;
    if ((x == 0 || layer[y][x - 1] == Empty_Tile) && !(tileMask & CGSME_PROP_MASK[CGSME_NUM_TILES][DIR_IDX_E]))
        return false;
    return true;
}

// remove mask cells that cant fit any valid tile against void/edges
static void pruneUnfillableMaskCells(cgsme_tile_t ***grid, uint32_t width, uint32_t length, uint32_t height)
{
    if (!cgsmeHasVoidRestrictions())
        return;

    uint8_t validByVoid[16];
    bool allValid = true;
    for (uint8_t v = 0; v < 16; v++)
    {
        cgsme_tile_t cand = All_Possible_State;
        for (int d = 0; d < 4; d++)
        {
            if (v & (1u << d))
                cand &= CGSME_PROP_MASK[CGSME_NUM_TILES][DIR_OPPOSITE(d)];
        }
        validByVoid[v] = (cand != Empty_Tile) ? 1u : 0u;
        if (!validByVoid[v])
            allValid = false;
    }
    if (allValid)
        return;

    uint32_t total = width * length;
    Point2D *q = NULL;
    uint8_t *inQ = NULL;

    static const int32_t dx[4] = { 0, 1, 0, -1 };
    static const int32_t dy[4] = { -1, 0, 1, 0 };

    uint32_t head = 0, tail = 0, count = 0;
    for (uint32_t y = 0; y < length; y++)
    {
        for (uint32_t x = 0; x < width; x++)
        {
            if (grid[0][y][x] != Empty_Tile && !validByVoid[layerVoidOobMask4(grid[0], width, length, x, y)])
            {
                if (!q)
                {
                    q = malloc(sizeof(Point2D) * total);
                    inQ = calloc(total, sizeof(uint8_t));
                    if (!q || !inQ)
                    {
                        free(q);
                        free(inQ);
                        return;
                    }
                }
                q[tail++] = (Point2D){ (int32_t)x, (int32_t)y };
                if (tail == total)
                    tail = 0;
                count++;
                inQ[y * width + x] = 1;
            }
        }
    }

    if (!q)
        return;

    while (count > 0)
    {
        Point2D c = q[head++];
        if (head == total)
            head = 0;
        count--;
        inQ[(uint32_t)c.y * width + (uint32_t)c.x] = 0;

        if (grid[0][c.y][c.x] == Empty_Tile)
            continue;
        if (validByVoid[layerVoidOobMask4(grid[0], width, length, (uint32_t)c.x, (uint32_t)c.y)])
            continue;

        for (uint32_t z = 0; z < height; z++)
            grid[z][c.y][c.x] = Empty_Tile;

        for (int d = 0; d < 4; d++)
        {
            int32_t nx = c.x + dx[d];
            int32_t ny = c.y + dy[d];
            if (nx < 0 || ny < 0 || (uint32_t)nx >= width || (uint32_t)ny >= length)
                continue;
            uint32_t nFlat = (uint32_t)ny * width + (uint32_t)nx;
            if (grid[0][ny][nx] != Empty_Tile && !inQ[nFlat])
            {
                q[tail++] = (Point2D){ nx, ny };
                if (tail == total)
                    tail = 0;
                count++;
                inQ[nFlat] = 1;
            }
        }
    }

    free(q);
    free(inQ);
}

static void clampPerimeterCell(cgsme_tile_t **gridLayer, uint32_t width, uint32_t length, uint32_t x, uint32_t y)
{
    if (CGSME_POPCOUNT(gridLayer[y][x]) <= 1)
        return;

    cgsme_tile_t cand = gridLayer[y][x];
    if (y == 0)
        cand &= CGSME_PROP_MASK[CGSME_NUM_TILES][DIR_IDX_S];
    if (y == length - 1)
        cand &= CGSME_PROP_MASK[CGSME_NUM_TILES][DIR_IDX_N];
    if (x == 0)
        cand &= CGSME_PROP_MASK[CGSME_NUM_TILES][DIR_IDX_E];
    if (x == width - 1)
        cand &= CGSME_PROP_MASK[CGSME_NUM_TILES][DIR_IDX_W];
    gridLayer[y][x] = cand;
}

static bool canPlaceQuotaTileAt(
    cgsme_tile_t ***grid, uint32_t width, uint32_t length, uint32_t height,
    uint32_t z, uint32_t x, uint32_t y,
    cgsme_tile_t lowerTile, cgsme_tile_t upMask, cgsme_tile_t downMask,
    int32_t spacing, bool allowStacking)
{
    if (x < 1 || y < 1 || x + 1 >= width || y + 1 >= length)
        return false;

    cgsme_tile_t cur = grid[z][y][x];
    if (cur == Empty_Tile || CGSME_POPCOUNT(cur) == 1 || (cur & lowerTile) == Empty_Tile)
        return false;

    if (!tileFitsVoidAndOob(grid[z], width, length, x, y, lowerTile))
        return false;

    if (z + 1 < height)
    {
        cgsme_tile_t above = grid[z + 1][y][x];
        cgsme_tile_t candUp = above & upMask;
        if (above == Empty_Tile || CGSME_POPCOUNT(above) == 1 || candUp == Empty_Tile)
            return false;

        bool anyUpFits = false;
        for (cgsme_tile_t tmp = candUp; tmp != 0; tmp &= (tmp - 1))
        {
            cgsme_tile_t b = CGSME_BIT(CGSME_CTZ(tmp));
            if (tileFitsVoidAndOob(grid[z + 1], width, length, x, y, b))
            {
                anyUpFits = true;
                break;
            }
        }
        if (!anyUpFits)
            return false;
    }

    if (z > 0)
    {
        cgsme_tile_t below = grid[z - 1][y][x];
        if (!allowStacking && below == lowerTile)
            return false;
        if (below != Empty_Tile && (below & downMask) == Empty_Tile)
            return false;
    }

    int32_t r = (spacing > 0) ? spacing : 1;
    for (int32_t dy = -r; dy <= r; dy++)
    {
        for (int32_t dx = -r; dx <= r; dx++)
        {
            int32_t nx = (int32_t)x + dx;
            int32_t ny = (int32_t)y + dy;
            if (nx < 0 || ny < 0 || (uint32_t)nx >= width || (uint32_t)ny >= length)
            {
                if (dy >= -1 && dy <= 1 && dx >= -1 && dx <= 1)
                    return false;
                continue;
            }
            if (dy >= -1 && dy <= 1 && dx >= -1 && dx <= 1)
            {
                if (grid[z][ny][nx] == Empty_Tile || (z + 1 < height && grid[z + 1][ny][nx] == Empty_Tile))
                    return false;
            }
            if (grid[z][ny][nx] != Empty_Tile && CGSME_POPCOUNT(grid[z][ny][nx]) == 1)
                return false;
            if (z + 1 < height && grid[z + 1][ny][nx] != Empty_Tile && CGSME_POPCOUNT(grid[z + 1][ny][nx]) == 1)
                return false;
        }
    }

    return true;
}

static void applyQuotaTileAt(
    cgsme_tile_t ***grid, uint32_t width, uint32_t length, uint32_t height,
    uint32_t z, uint32_t x, uint32_t y,
    cgsme_tile_t lowerTile, cgsme_tile_t upMask, cgsme_tile_t downMask)
{
    (void)width;
    (void)length;
    grid[z][y][x] = lowerTile;
    if (z + 1 < height)
        grid[z + 1][y][x] &= upMask;
    if (z > 0 && grid[z - 1][y][x] != Empty_Tile)
        grid[z - 1][y][x] &= downMask;
}

// architect: mask setup + nxn area quota pre-seeding before threads start
void runArchitect(cgsme_tile_t ***grid, uint32_t width, uint32_t length, uint32_t height, uint32_t fulness, uint32_t seed)
{
    CGSME_PROFILE_FUNC();

    uint32_t rngState = seed;

    if (fulness < 100)
    {
        generateRidgedMask(grid, width, length, height, fulness, seed);
    }
    else
    {
        for (uint32_t z = 0; z < height; z++)
            for (uint32_t y = 0; y < length; y++)
                for (uint32_t x = 0; x < width; x++)
                    grid[z][y][x] = All_Possible_State;
    }

    pruneUnfillableMaskCells(grid, width, length, height);

    if (cgsmeHasVoidRestrictions())
    {
        for (uint32_t z = 0; z < height; z++)
        {
            for (uint32_t x = 0; x < width; x++)
            {
                clampPerimeterCell(grid[z], width, length, x, 0);
                if (length > 1)
                    clampPerimeterCell(grid[z], width, length, x, length - 1);
            }
            for (uint32_t y = 1; y + 1 < length; y++)
            {
                clampPerimeterCell(grid[z], width, length, 0, y);
                if (width > 1)
                    clampPerimeterCell(grid[z], width, length, width - 1, y);
            }
        }
    }

    if (height > 1)
    {
        cgsme_tile_t bottomMask = CGSME_VERT_PROP_MASK[CGSME_NUM_TILES][VERT_IDX_UP];
        cgsme_tile_t topMask    = CGSME_VERT_PROP_MASK[CGSME_NUM_TILES][VERT_IDX_DOWN];
        for (uint32_t y = 0; y < length; y++)
        {
            for (uint32_t x = 0; x < width; x++)
            {
                if (grid[0][y][x] != Empty_Tile)
                    grid[0][y][x] &= bottomMask;
                if (grid[height - 1][y][x] != Empty_Tile)
                    grid[height - 1][y][x] &= topMask;
            }
        }
    }

    // nxn area quota placement + layer connectivity
    for (int a = 0; a < CGSME_NUM_ANCHORS; a++)
    {
        uint8_t lowerIdx = CGSME_ANCHOR_LOWER[a];
        cgsme_tile_t lowerTile = CGSME_BIT(lowerIdx);
        cgsme_tile_t upMask    = CGSME_VERT_PROP_MASK[lowerIdx][VERT_IDX_UP];
        cgsme_tile_t downMask  = CGSME_VERT_PROP_MASK[lowerIdx][VERT_IDX_DOWN];
        uint32_t areaSize      = (CGSME_ANCHOR_AREA_SIZE[a] >= 4) ? CGSME_ANCHOR_AREA_SIZE[a] : 20u;
        uint32_t countPerArea  = CGSME_ANCHOR_COUNT_PER_AREA[a];
        uint32_t minPerLayer   = CGSME_ANCHOR_MIN[a];
        int32_t  spacing       = (int32_t)CGSME_ANCHOR_SPACING[a];
        bool     allowStacking = CGSME_ANCHOR_ALLOW_STACKING[a];

        uint32_t anchorLayers = (height > 1) ? (height - 1) : ((cgsmeHasAnyOpenPort() == Empty_Tile) ? 1u : 0u);
        if (height > 1 && (CGSME_VERT_PROP_MASK[CGSME_NUM_TILES][VERT_IDX_DOWN] & lowerTile) != Empty_Tile && cgsmeHasAnyOpenPort() == Empty_Tile)
            anchorLayers = height;

        uint32_t areasX = (width + areaSize - 1) / areaSize;
        uint32_t areasY = (length + areaSize - 1) / areaSize;

        for (uint32_t z = 0; z < anchorLayers; z++)
        {
            int placedOnLayer = 0;

            // per area placement
            for (uint32_t ay = 0; ay < areasY; ay++)
            {
                uint32_t y0 = ay * areaSize;
                uint32_t y1 = (y0 + areaSize < length) ? (y0 + areaSize - 1) : (length - 1);
                if (y0 < 1) y0 = 1;
                if (y1 >= length - 1) y1 = length - 2;
                if (y0 > y1) continue;

                for (uint32_t ax = 0; ax < areasX; ax++)
                {
                    uint32_t x0 = ax * areaSize;
                    uint32_t x1 = (x0 + areaSize < width) ? (x0 + areaSize - 1) : (width - 1);
                    if (x0 < 1) x0 = 1;
                    if (x1 >= width - 1) x1 = width - 2;
                    if (x0 > x1) continue;

                    uint32_t spanX = x1 - x0 + 1;
                    uint32_t spanY = y1 - y0 + 1;

                    for (uint32_t k = 0; k < countPerArea; k++)
                    {
                        bool placedOne = false;
                        for (int attempt = 0; attempt < 20; attempt++)
                        {
                            uint32_t rx = x0 + (nextRandom(&rngState) % spanX);
                            uint32_t ry = y0 + (nextRandom(&rngState) % spanY);
                            if (canPlaceQuotaTileAt(grid, width, length, height, z, rx, ry, lowerTile, upMask, downMask, spacing, allowStacking))
                            {
                                applyQuotaTileAt(grid, width, length, height, z, rx, ry, lowerTile, upMask, downMask);
                                placedOnLayer++;
                                placedOne = true;
                                break;
                            }
                        }

                        if (!placedOne)
                        {
                            // fallback scan over valid spots in this area
                            uint32_t seenValid = 0;
                            uint32_t pickX = 0, pickY = 0;
                            for (uint32_t sy = y0; sy <= y1; sy++)
                            {
                                for (uint32_t sx = x0; sx <= x1; sx++)
                                {
                                    if (canPlaceQuotaTileAt(grid, width, length, height, z, sx, sy, lowerTile, upMask, downMask, spacing, allowStacking))
                                    {
                                        seenValid++;
                                        if ((nextRandom(&rngState) % seenValid) == 0)
                                        {
                                            pickX = sx;
                                            pickY = sy;
                                        }
                                    }
                                }
                            }
                            if (seenValid > 0)
                            {
                                applyQuotaTileAt(grid, width, length, height, z, pickX, pickY, lowerTile, upMask, downMask);
                                placedOnLayer++;
                            }
                        }
                    }
                }
            }

            // fallback to hit min_per_layer
            while (placedOnLayer < (int)minPerLayer)
            {
                uint32_t seenValid = 0;
                uint32_t pickX = 0, pickY = 0;
                int32_t fallbackSpacing = (spacing > 1) ? 1 : spacing;
                for (uint32_t sy = 1; sy + 1 < length; sy++)
                {
                    for (uint32_t sx = 1; sx + 1 < width; sx++)
                    {
                        if (canPlaceQuotaTileAt(grid, width, length, height, z, sx, sy, lowerTile, upMask, downMask, fallbackSpacing, allowStacking))
                        {
                            seenValid++;
                            if ((nextRandom(&rngState) % seenValid) == 0)
                            {
                                pickX = sx;
                                pickY = sy;
                            }
                        }
                    }
                }
                if (seenValid > 0)
                {
                    applyQuotaTileAt(grid, width, length, height, z, pickX, pickY, lowerTile, upMask, downMask);
                    placedOnLayer++;
                }
                else
                {
                    break;
                }
            }

            // if 0 connectors placed between z and z+1, force a 3x3 pad so layers still connect
            if (placedOnLayer == 0 && CGSME_GUARANTEE_LAYER_CONN && z + 1 < height && width >= 4 && length >= 4)
            {
                uint32_t bx = width / 2;
                uint32_t by = length / 2;
                bool foundPad = false;
                for (uint32_t sy = 2; sy + 2 < length && !foundPad; sy++)
                {
                    for (uint32_t sx = 2; sx + 2 < width; sx++)
                    {
                        if (grid[z][sy][sx] != Empty_Tile || grid[z + 1][sy][sx] != Empty_Tile)
                        {
                            if (z == 0 || allowStacking || grid[z - 1][sy][sx] != lowerTile)
                            {
                                bx = sx;
                                by = sy;
                                foundPad = true;
                                break;
                            }
                        }
                    }
                }
                for (int32_t dy = -1; dy <= 1; dy++)
                {
                    for (int32_t dx = -1; dx <= 1; dx++)
                    {
                        uint32_t nx = (uint32_t)((int32_t)bx + dx);
                        uint32_t ny = (uint32_t)((int32_t)by + dy);
                        if (grid[z][ny][nx] == Empty_Tile)
                            grid[z][ny][nx] = All_Possible_State;
                        if (grid[z + 1][ny][nx] == Empty_Tile)
                            grid[z + 1][ny][nx] = All_Possible_State;
                    }
                }
                applyQuotaTileAt(grid, width, length, height, z, bx, by, lowerTile, upMask, downMask);
            }
        }
    }

    // remove non-natural tiles from uncollapsed cells so wfc wont spawn them
    cgsme_tile_t naturalMask = Empty_Tile;
    for (int t = 0; t < CGSME_NUM_TILES; t++)
    {
        if (CGSME_CATEGORY_NATURAL[CGSME_BIT_TO_CATEGORY[t]])
            naturalMask |= CGSME_BIT(t);
    }
    if (naturalMask != Empty_Tile && naturalMask != All_Possible_State)
    {
        for (uint32_t z = 0; z < height; z++)
        {
            for (uint32_t y = 0; y < length; y++)
            {
                for (uint32_t x = 0; x < width; x++)
                {
                    if (CGSME_POPCOUNT(grid[z][y][x]) > 1)
                        grid[z][y][x] &= naturalMask;
                }
            }
        }
    }
}

static int countCollapsedCells(cgsme_tile_t **gridLayer, uint32_t width, uint32_t length)
{
    int count = 0;
    for (uint32_t y = 0; y < length; y++)
    {
        for (uint32_t x = 0; x < width; x++)
        {
            if (gridLayer[y][x] != Empty_Tile && CGSME_POPCOUNT(gridLayer[y][x]) == 1)
                count++;
        }
    }
    return count;
}


int generateLayerThread(void *args)
{
    CGSME_PROFILE_FUNC();
    layerGenerationArgs *arg = (layerGenerationArgs *)args;
    cgsme_tile_t **gridLayer = arg->gridLayer;
    uint32_t width = arg->width;
    uint32_t length = arg->length;
    int32_t startX = arg->startX;
    int32_t startY = arg->startY;
    uint32_t seed = arg->seed;
    uint32_t fulness = arg->fulness;

    uint32_t rngState = seed;

    cgsme_tile_t anyOpen = cgsmeHasAnyOpenPort();
    cgsme_tile_t allPortTiles = CGSME_HAS_PORT_MASK[0] | CGSME_HAS_PORT_MASK[1] |
                                CGSME_HAS_PORT_MASK[2] | CGSME_HAS_PORT_MASK[3];
    bool hasZeroPortTiles = ((All_Possible_State & ~allPortTiles) != Empty_Tile);

    // WEIGHTS CONFIGURATION
    float current_spawnrates[CGSME_MAX_CATEGORIES];
    if (fulness < 100 || anyOpen == Empty_Tile)
    {
        // mask mode: use mask_weight from json
        for (int i = 0; i < CGSME_NUM_CATEGORIES; ++i)
            current_spawnrates[i] = CGSME_CATEGORY_MASK_WEIGHT[i];
    }
    else
    {
        // ocean mode: uniform start across natural categories
        int naturalCount = 0;
        for (int i = 0; i < CGSME_NUM_CATEGORIES; ++i)
            if (CGSME_CATEGORY_NATURAL[i])
                naturalCount++;

        for (int i = 0; i < CGSME_NUM_CATEGORIES; ++i)
            current_spawnrates[i] = (CGSME_CATEGORY_NATURAL[i] && naturalCount > 0) ? (1.0f / (float)naturalCount) : 0.0f;
    }

    if (cgsmeHasVoidRestrictions())
    {
        for (uint32_t x = 0; x < width; x++)
        {
            clampPerimeterCell(gridLayer, width, length, x, 0);
            if (length > 1)
                clampPerimeterCell(gridLayer, width, length, x, length - 1);
        }
        for (uint32_t y = 1; y + 1 < length; y++)
        {
            clampPerimeterCell(gridLayer, width, length, 0, y);
            if (width > 1)
                clampPerimeterCell(gridLayer, width, length, width - 1, y);
        }
    }

    // EXACT TARGET COUNTING
    // exact mask size
    int target_collapsed_count = 0;
    for (uint32_t y = 0; y < length; y++)
    {
        for (uint32_t x = 0; x < width; x++)
        {
            if (gridLayer[y][x] != Empty_Tile)
                target_collapsed_count++;
        }
    }

    int valid_collapsed_count = 0;

    MinHeap *heap = initHeap(width, length);

    // INIT and CONSTRAINT PROPAGATION
    for (uint32_t i = 0; i < length; i++)
    {
        for (uint32_t j = 0; j < width; j++)
        {
            if (gridLayer[i][j] == Empty_Tile || CGSME_POPCOUNT(gridLayer[i][j]) == 1)
            {
                // Void cells and preplaced anchors propagate constraints to neighbors
                updateNeighbours(gridLayer, width, length, j, i, heap, &rngState);
            }
        }
    }

    valid_collapsed_count = countCollapsedCells(gridLayer, width, length);

    // SEED CENTER (if valid)
    if (gridLayer[startY][startX] != Empty_Tile && CGSME_POPCOUNT(gridLayer[startY][startX]) > 1)
    {
        if (gridLayer[startY][startX] & CGSME_SEED_TILE_MASK)
            gridLayer[startY][startX] = CGSME_SEED_TILE_MASK;
        else
            collapseTile(&gridLayer[startY][startX], current_spawnrates, &rngState);

        updateNeighbours(gridLayer, width, length, startX, startY, heap, &rngState);
        valid_collapsed_count++;

        // add neighbors to heap to kickstart
        if (startY > 0)
            heapInsertOrUpdate(heap, gridLayer, startX, startY - 1, &rngState);
        if (startY < length - 1)
            heapInsertOrUpdate(heap, gridLayer, startX, startY + 1, &rngState);
        if (startX > 0)
            heapInsertOrUpdate(heap, gridLayer, startX - 1, startY, &rngState);
        if (startX < width - 1)
            heapInsertOrUpdate(heap, gridLayer, startX + 1, startY, &rngState);
    }

    // high safety limit for complex masks
    int max_iter = width * length * 50;
    int iter = 0;

    // MAIN LOOP
    while (valid_collapsed_count < target_collapsed_count && iter < max_iter)
    {
        iter++;

        // only use dynamic pacing if NOT in mask mode (and only for open-port mazes)
        if (fulness >= 100 && anyOpen != Empty_Tile && (iter % 64 == 0 || valid_collapsed_count < 50))
        {
            update_spawnrates(current_spawnrates, valid_collapsed_count, target_collapsed_count);
        }

        uint32_t cx, cy;
        bool found = heapPop(heap, gridLayer, &cx, &cy);

        if (!found)
        {
            // HEAP EMPTY: reseed using AGGRESSIVE finder
            // this will pick any tile inside the mask that isnt solved yet
            if (findBestSeedLocation(gridLayer, width, length, &cx, &cy, &rngState))
            {
                found = true;

                if (CGSME_POPCOUNT(gridLayer[cy][cx]) > 1)
                {
                    if (gridLayer[cy][cx] & CGSME_SEED_TILE_MASK)
                        gridLayer[cy][cx] = CGSME_SEED_TILE_MASK;
                    else
                        collapseTile(&gridLayer[cy][cx], current_spawnrates, &rngState);

                    updateNeighbours(gridLayer, width, length, cx, cy, heap, &rngState);
                    valid_collapsed_count++;

                    // Add neighbors
                    if (cy > 0)
                        heapInsertOrUpdate(heap, gridLayer, cx, cy - 1, &rngState);
                    if (cy < length - 1)
                        heapInsertOrUpdate(heap, gridLayer, cx, cy + 1, &rngState);
                    if (cx > 0)
                        heapInsertOrUpdate(heap, gridLayer, cx - 1, cy, &rngState);
                    if (cx < width - 1)
                        heapInsertOrUpdate(heap, gridLayer, cx + 1, cy, &rngState);

                    continue; // skip the collapse step for this iteration
                }
                else if (CGSME_POPCOUNT(gridLayer[cy][cx]) == 1)
                {
                    updateNeighbours(gridLayer, width, length, cx, cy, heap, &rngState);
                    valid_collapsed_count++;
                    continue;
                }
            }
            else
            {
                if (CGSME_NUM_SOCKETS > 2 && iter < max_iter)
                {
                    bool reopened = false;
                    for (uint32_t ry = 0; ry < length && !reopened; ry++)
                    {
                        for (uint32_t rx = 0; rx < width; rx++)
                        {
                            cgsme_tile_t c = gridLayer[ry][rx];
                            if (c == Empty_Tile)
                                continue;
                            if (CGSME_POPCOUNT(c) != 1 || (c & exactCellValidMask(gridLayer, width, length, rx, ry)) == Empty_Tile)
                            {
                                backtrackNeighborhood(gridLayer, width, length, rx, ry, heap, &rngState);
                                reopened = true;
                                break;
                            }
                        }
                    }
                    if (reopened)
                    {
                        valid_collapsed_count = countCollapsedCells(gridLayer, width, length);
                        if (valid_collapsed_count >= target_collapsed_count)
                            valid_collapsed_count = target_collapsed_count - 1;
                        continue;
                    }
                }
                // no more tiles left to fill
                break;
            }
        }

        // collapse
        if (CGSME_POPCOUNT(gridLayer[cy][cx]) > 1)
        {
            collapseTile(&gridLayer[cy][cx], current_spawnrates, &rngState);
            updateNeighbours(gridLayer, width, length, cx, cy, heap, &rngState);

            // because updateNeighbours now revives dead tiles, gridLayer will never be Empty_Tile
            // unless it was mask void. It will be All_Possible if it failed
            if (gridLayer[cy][cx] != Empty_Tile && CGSME_POPCOUNT(gridLayer[cy][cx]) == 1)
                valid_collapsed_count++;
        }

        // add neighbors to heap
        // only add if they are still uncollapsed candidates
        if (cy > 0 && CGSME_POPCOUNT(gridLayer[cy - 1][cx]) > 1)
            heapInsertOrUpdate(heap, gridLayer, cx, cy - 1, &rngState);
        if (cy < length - 1 && CGSME_POPCOUNT(gridLayer[cy + 1][cx]) > 1)
            heapInsertOrUpdate(heap, gridLayer, cx, cy + 1, &rngState);
        if (cx > 0 && CGSME_POPCOUNT(gridLayer[cy][cx - 1]) > 1)
            heapInsertOrUpdate(heap, gridLayer, cx - 1, cy, &rngState);
        if (cx < width - 1 && CGSME_POPCOUNT(gridLayer[cy][cx + 1]) > 1)
            heapInsertOrUpdate(heap, gridLayer, cx + 1, cy, &rngState);

        // VOID LOGIC (only for ocean mode on rulesets without 0-port tiles)
        // if hit target count in nonmasked mode start deleting unnecessary tiles
        if (!hasZeroPortTiles && fulness >= 100 && valid_collapsed_count >= target_collapsed_count)
        {
            if (!isTileRequired(gridLayer, width, length, cx, cy))
            {
                gridLayer[cy][cx] = Empty_Tile;
                updateNeighbours(gridLayer, width, length, cx, cy, heap, &rngState);
                valid_collapsed_count--; // adjust count
            }
        }

        if (CGSME_NUM_SOCKETS > 2 && valid_collapsed_count >= target_collapsed_count && iter < max_iter)
        {
            valid_collapsed_count = countCollapsedCells(gridLayer, width, length);
            if (valid_collapsed_count >= target_collapsed_count)
            {
                for (uint32_t ry = 0; ry < length; ry++)
                {
                    for (uint32_t rx = 0; rx < width; rx++)
                    {
                        cgsme_tile_t c = gridLayer[ry][rx];
                        if (c == Empty_Tile)
                            continue;
                        if (CGSME_POPCOUNT(c) != 1 || (c & exactCellValidMask(gridLayer, width, length, rx, ry)) == Empty_Tile)
                        {
                            backtrackNeighborhood(gridLayer, width, length, rx, ry, heap, &rngState);
                            valid_collapsed_count = countCollapsedCells(gridLayer, width, length);
                            if (valid_collapsed_count >= target_collapsed_count)
                                valid_collapsed_count = target_collapsed_count - 1;
                            ry = length;
                            break;
                        }
                    }
                }
            }
        }
    }

    // CLEANUP & WELDING
    for (uint32_t i = 0; i < length; i++)
    {
        for (uint32_t j = 0; j < width; j++)
        {
            // if anything is still superposition (shouldn't be), kill it
            if (CGSME_POPCOUNT(gridLayer[i][j]) > 1)
                gridLayer[i][j] = Empty_Tile;
        }
    }

    if (anyOpen != Empty_Tile && CGSME_NUM_SOCKETS <= 2)
    {
        fixupEdges(gridLayer, width, length);
        sealMazeEdges(gridLayer, width, length);
        repairMazePortsInPlace(gridLayer, width, length);

        bool allConnected = true;
        bool postWeldRepaired = false;
        for (int weldPass = 0; weldPass < 3; weldPass++)
        {
            bool usedSuperset = false;
            findConnectedRegionsInPlace(gridLayer, width, length);
            germanWelderExInPlace(gridLayer, width, length, &rngState, &allConnected, &usedSuperset);

            for (uint32_t i = 0; i < length; i++)
            {
                for (uint32_t j = 0; j < width; j++)
                {
                    if (gridLayer[i][j] == CGSME_PACKED_VOID)
                        gridLayer[i][j] = Empty_Tile;
                    else
                    {
                        uint8_t index = (uint8_t)(gridLayer[i][j] & CGSME_TILE_INDEX_MASK);
                        gridLayer[i][j] = indexToMask(index);
                    }
                }
            }

            if (!usedSuperset)
            {
                postWeldRepaired = false;
                break;
            }

            postWeldRepaired = repairMazePortsInPlace(gridLayer, width, length);
            if (!postWeldRepaired)
                break;
        }

        if (!allConnected || postWeldRepaired)
            pruneUnreachableComponentsInPlace(gridLayer, width, length);
    }

    // free memory
    freeHeap(heap);

    return 0;
}

typedef struct
{
    uint32_t cx, cy;
    uint32_t x0, y0, x1, y1;
    uint32_t chunkSeed;
} CheckerChunkTask;


static int countChunkCollapsed3D(
    cgsme_tile_t ***grid, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t height)
{
    int count = 0;
    for (uint32_t z = 0; z < height; z++)
    {
        for (uint32_t y = y0; y <= y1; y++)
        {
            for (uint32_t x = x0; x <= x1; x++)
            {
                if (grid[z][y][x] != Empty_Tile && CGSME_POPCOUNT(grid[z][y][x]) == 1)
                    count++;
            }
        }
    }
    return count;
}

static void solveColumnChunk3D(
    cgsme_tile_t ***grid, uint32_t width, uint32_t length, uint32_t height,
    uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
    uint32_t chunkSeed, uint32_t fulness, ChunkHeap3D *heap)
{
    uint32_t rngState = chunkSeed;
    uint32_t cw = x1 - x0 + 1;
    uint32_t cl = y1 - y0 + 1;

    bindChunkHeap3D(heap, x0, y0, cw, cl, height);

    cgsme_tile_t anyOpen = cgsmeHasAnyOpenPort();
    cgsme_tile_t allPortTiles = CGSME_HAS_PORT_MASK[0] | CGSME_HAS_PORT_MASK[1] |
                                CGSME_HAS_PORT_MASK[2] | CGSME_HAS_PORT_MASK[3];
    bool hasZeroPortTiles = ((All_Possible_State & ~allPortTiles) != Empty_Tile);
    bool isMultiRule = (CGSME_NUM_SOCKETS > 2);
    bool hasVertRules = (CGSME_VERT_PROP_MASK[0][0] != All_Possible_State &&
                         CGSME_VERT_PROP_MASK[0][1] != All_Possible_State);

    float current_spawnrates[CGSME_MAX_CATEGORIES];
    if (fulness < 100 || anyOpen == Empty_Tile || isMultiRule || hasVertRules)
    {
        for (int i = 0; i < CGSME_NUM_CATEGORIES; ++i)
            current_spawnrates[i] = CGSME_CATEGORY_MASK_WEIGHT[i];
    }
    else
    {
        int naturalCount = 0;
        for (int i = 0; i < CGSME_NUM_CATEGORIES; ++i)
            if (CGSME_CATEGORY_NATURAL[i])
                naturalCount++;

        for (int i = 0; i < CGSME_NUM_CATEGORIES; ++i)
            current_spawnrates[i] = (CGSME_CATEGORY_NATURAL[i] && naturalCount > 0) ? (1.0f / (float)naturalCount) : 0.0f;
    }

    int target_collapsed_count = 0;

    // pull constraints from void and already collapsed neighbor chunks
    for (uint32_t z = 0; z < height; z++)
    {
        for (uint32_t y = y0; y <= y1; y++)
        {
            for (uint32_t x = x0; x <= x1; x++)
            {
                cgsme_tile_t cell = grid[z][y][x];
                if (cell == Empty_Tile)
                    continue;
                target_collapsed_count++;

                if (CGSME_POPCOUNT(cell) > 1)
                {
                    cgsme_tile_t valid = exactCellValidMask3D(grid, width, length, height, x, y, z);
                    if ((cell & valid) != Empty_Tile)
                        grid[z][y][x] = cell & valid;
                    else if (valid != Empty_Tile)
                        grid[z][y][x] = valid;
                }
            }
        }
    }

    if (target_collapsed_count == 0)
        return;

    // propagate void, anchors and chunk border constraints
    for (uint32_t z = 0; z < height; z++)
    {
        for (uint32_t y = y0; y <= y1; y++)
        {
            for (uint32_t x = x0; x <= x1; x++)
            {
                cgsme_tile_t cell = grid[z][y][x];
                if (cell == Empty_Tile || CGSME_POPCOUNT(cell) == 1)
                {
                    updateNeighboursChunk3D(grid, width, length, height, heap, x, y, z, &rngState);
                }
                else if (x == x0 || x == x1 || y == y0 || y == y1)
                {
                    if ((x == x0 && x0 > 0 && CGSME_POPCOUNT(grid[z][y][x0 - 1]) == 1) ||
                        (x == x1 && x1 + 1 < width && CGSME_POPCOUNT(grid[z][y][x1 + 1]) == 1) ||
                        (y == y0 && y0 > 0 && CGSME_POPCOUNT(grid[z][y0 - 1][x]) == 1) ||
                        (y == y1 && y1 + 1 < length && CGSME_POPCOUNT(grid[z][y1 + 1][x]) == 1))
                    {
                        chunkHeapInsertOrUpdate3D(heap, grid, x, y, z, &rngState);
                        if (CGSME_NUM_SOCKETS > 2)
                            updateNeighboursChunk3D(grid, width, length, height, heap, x, y, z, &rngState);
                    }
                }
            }
        }
    }

    int valid_collapsed_count = countChunkCollapsed3D(grid, x0, y0, x1, y1, height);

    // seed chunk center if nothing is in the heap yet (phase 0 chunks)
    if (heap->count == 0)
    {
        uint32_t midX = x0 + cw / 2;
        uint32_t midY = y0 + cl / 2;
        for (uint32_t z = 0; z < height; z++)
        {
            if (grid[z][midY][midX] != Empty_Tile && CGSME_POPCOUNT(grid[z][midY][midX]) > 1)
            {
                if (grid[z][midY][midX] & CGSME_SEED_TILE_MASK)
                    grid[z][midY][midX] = CGSME_SEED_TILE_MASK;
                else
                    collapseTile(&grid[z][midY][midX], current_spawnrates, &rngState);

                updateNeighboursChunk3D(grid, width, length, height, heap, midX, midY, z, &rngState);
                valid_collapsed_count++;

                if (midY > y0)
                    chunkHeapInsertOrUpdate3D(heap, grid, midX, midY - 1, z, &rngState);
                if (midY < y1)
                    chunkHeapInsertOrUpdate3D(heap, grid, midX, midY + 1, z, &rngState);
                if (midX > x0)
                    chunkHeapInsertOrUpdate3D(heap, grid, midX - 1, midY, z, &rngState);
                if (midX < x1)
                    chunkHeapInsertOrUpdate3D(heap, grid, midX + 1, midY, z, &rngState);
                if (z > 0)
                    chunkHeapInsertOrUpdate3D(heap, grid, midX, midY, z - 1, &rngState);
                if (z + 1 < height)
                    chunkHeapInsertOrUpdate3D(heap, grid, midX, midY, z + 1, &rngState);
            }
        }
    }

    int max_iter = (int)(cw * cl * height * 50u);
    int iter = 0;

    // main 3d wfc loop for this column chunk
    while (valid_collapsed_count < target_collapsed_count && iter < max_iter)
    {
        iter++;

        if (fulness >= 100 && anyOpen != Empty_Tile && !hasVertRules && (iter % 32 == 0 || valid_collapsed_count < 25))
        {
            update_spawnrates(current_spawnrates, valid_collapsed_count, target_collapsed_count);
        }

        uint32_t cx, cy, cz;
        bool found = chunkHeapPop3D(heap, grid, &cx, &cy, &cz);

        if (!found)
        {
            if (findBestSeedLocationChunk3D(grid, width, length, height, heap, &cx, &cy, &cz, &rngState))
            {
                if (CGSME_POPCOUNT(grid[cz][cy][cx]) > 1)
                {
                    if (grid[cz][cy][cx] & CGSME_SEED_TILE_MASK)
                        grid[cz][cy][cx] = CGSME_SEED_TILE_MASK;
                    else
                        collapseTile(&grid[cz][cy][cx], current_spawnrates, &rngState);

                    updateNeighboursChunk3D(grid, width, length, height, heap, cx, cy, cz, &rngState);
                    valid_collapsed_count++;

                    if (cy > y0)
                        chunkHeapInsertOrUpdate3D(heap, grid, cx, cy - 1, cz, &rngState);
                    if (cy < y1)
                        chunkHeapInsertOrUpdate3D(heap, grid, cx, cy + 1, cz, &rngState);
                    if (cx > x0)
                        chunkHeapInsertOrUpdate3D(heap, grid, cx - 1, cy, cz, &rngState);
                    if (cx < x1)
                        chunkHeapInsertOrUpdate3D(heap, grid, cx + 1, cy, cz, &rngState);
                    if (cz > 0)
                        chunkHeapInsertOrUpdate3D(heap, grid, cx, cy, cz - 1, &rngState);
                    if (cz + 1 < height)
                        chunkHeapInsertOrUpdate3D(heap, grid, cx, cy, cz + 1, &rngState);
                    continue;
                }
                else if (CGSME_POPCOUNT(grid[cz][cy][cx]) == 1)
                {
                    updateNeighboursChunk3D(grid, width, length, height, heap, cx, cy, cz, &rngState);
                    valid_collapsed_count++;
                    continue;
                }
            }
            else
            {
                if (isMultiRule && iter < max_iter)
                {
                    bool reopened = false;
                    for (uint32_t rz = 0; rz < height && !reopened; rz++)
                    {
                        for (uint32_t ry = y0; ry <= y1 && !reopened; ry++)
                        {
                            for (uint32_t rx = x0; rx <= x1; rx++)
                            {
                                cgsme_tile_t c = grid[rz][ry][rx];
                                if (c == Empty_Tile)
                                    continue;
                                if (CGSME_POPCOUNT(c) != 1 || (c & exactCellValidMask3D(grid, width, length, height, rx, ry, rz)) == Empty_Tile)
                                {
                                    backtrackChunkNeighborhood3D(grid, width, length, height, heap, rx, ry, rz, &rngState);
                                    reopened = true;
                                    break;
                                }
                            }
                        }
                    }
                    if (reopened)
                    {
                        valid_collapsed_count = countChunkCollapsed3D(grid, x0, y0, x1, y1, height);
                        if (valid_collapsed_count >= target_collapsed_count)
                            valid_collapsed_count = target_collapsed_count - 1;
                        continue;
                    }
                }
                break;
            }
        }

        if (CGSME_POPCOUNT(grid[cz][cy][cx]) > 1)
        {
            collapseTile(&grid[cz][cy][cx], current_spawnrates, &rngState);
            updateNeighboursChunk3D(grid, width, length, height, heap, cx, cy, cz, &rngState);
            if (grid[cz][cy][cx] != Empty_Tile && CGSME_POPCOUNT(grid[cz][cy][cx]) == 1)
                valid_collapsed_count++;
        }

        if (cy > y0 && CGSME_POPCOUNT(grid[cz][cy - 1][cx]) > 1)
            chunkHeapInsertOrUpdate3D(heap, grid, cx, cy - 1, cz, &rngState);
        if (cy < y1 && CGSME_POPCOUNT(grid[cz][cy + 1][cx]) > 1)
            chunkHeapInsertOrUpdate3D(heap, grid, cx, cy + 1, cz, &rngState);
        if (cx > x0 && CGSME_POPCOUNT(grid[cz][cy][cx - 1]) > 1)
            chunkHeapInsertOrUpdate3D(heap, grid, cx - 1, cy, cz, &rngState);
        if (cx < x1 && CGSME_POPCOUNT(grid[cz][cy][cx + 1]) > 1)
            chunkHeapInsertOrUpdate3D(heap, grid, cx + 1, cy, cz, &rngState);
        if (cz > 0 && CGSME_POPCOUNT(grid[cz - 1][cy][cx]) > 1)
            chunkHeapInsertOrUpdate3D(heap, grid, cx, cy, cz - 1, &rngState);
        if (cz + 1 < height && CGSME_POPCOUNT(grid[cz + 1][cy][cx]) > 1)
            chunkHeapInsertOrUpdate3D(heap, grid, cx, cy, cz + 1, &rngState);

        if (!hasZeroPortTiles && fulness >= 100 && valid_collapsed_count >= target_collapsed_count)
        {
            if (!isTileRequired(grid[cz], width, length, cx, cy))
            {
                grid[cz][cy][cx] = Empty_Tile;
                updateNeighboursChunk3D(grid, width, length, height, heap, cx, cy, cz, &rngState);
                valid_collapsed_count--;
            }
        }

        if (isMultiRule && valid_collapsed_count >= target_collapsed_count && iter < max_iter)
        {
            valid_collapsed_count = countChunkCollapsed3D(grid, x0, y0, x1, y1, height);
            if (valid_collapsed_count >= target_collapsed_count)
            {
                bool reopened = false;
                for (uint32_t rz = 0; rz < height && !reopened; rz++)
                {
                    for (uint32_t ry = y0; ry <= y1 && !reopened; ry++)
                    {
                        for (uint32_t rx = x0; rx <= x1; rx++)
                        {
                            cgsme_tile_t c = grid[rz][ry][rx];
                            if (c == Empty_Tile)
                                continue;
                            if (CGSME_POPCOUNT(c) != 1 || (c & exactCellValidMask3D(grid, width, length, height, rx, ry, rz)) == Empty_Tile)
                            {
                                backtrackChunkNeighborhood3D(grid, width, length, height, heap, rx, ry, rz, &rngState);
                                valid_collapsed_count = countChunkCollapsed3D(grid, x0, y0, x1, y1, height);
                                if (valid_collapsed_count >= target_collapsed_count)
                                    valid_collapsed_count = target_collapsed_count - 1;
                                reopened = true;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
}

static void finalizeLayerSeamsAndWeld(
    cgsme_tile_t ***grid, uint32_t width, uint32_t length, uint32_t height,
    uint32_t z, uint32_t layerSeed, uint32_t fulness)
{
    cgsme_tile_t **gridLayer = grid[z];
    uint32_t rngState = layerSeed;
    cgsme_tile_t anyOpen = cgsmeHasAnyOpenPort();

    if (CGSME_NUM_SOCKETS > 2)
    {
        float current_spawnrates[CGSME_MAX_CATEGORIES];
        for (int i = 0; i < CGSME_NUM_CATEGORIES; ++i)
            current_spawnrates[i] = CGSME_CATEGORY_MASK_WEIGHT[i];

        MinHeap *seamHeap = NULL;
        int maxSeamIter = (int)(width * length * 20u);
        int seamIter = 0;

        while (seamIter < maxSeamIter)
        {
            bool foundMismatch = false;
            for (uint32_t y = 0; y < length && !foundMismatch; y++)
            {
                for (uint32_t x = 0; x < width; x++)
                {
                    cgsme_tile_t c = gridLayer[y][x];
                    if (c == Empty_Tile)
                        continue;
                    if (CGSME_POPCOUNT(c) != 1 || (c & exactCellValidMask3D(grid, width, length, height, x, y, z)) == Empty_Tile)
                    {
                        if (!seamHeap)
                            seamHeap = initHeap(width, length);
                        backtrackNeighborhood(gridLayer, width, length, x, y, seamHeap, &rngState);
                        foundMismatch = true;
                        break;
                    }
                }
            }

            if (!foundMismatch)
                break;

            uint32_t cx, cy;
            while (seamIter < maxSeamIter && heapPop(seamHeap, gridLayer, &cx, &cy))
            {
                seamIter++;
                if (CGSME_POPCOUNT(gridLayer[cy][cx]) > 1)
                {
                    collapseTile(&gridLayer[cy][cx], current_spawnrates, &rngState);
                    updateNeighbours(gridLayer, width, length, cx, cy, seamHeap, &rngState);
                }
                if (cy > 0 && CGSME_POPCOUNT(gridLayer[cy - 1][cx]) > 1)
                    heapInsertOrUpdate(seamHeap, gridLayer, cx, cy - 1, &rngState);
                if (cy + 1 < length && CGSME_POPCOUNT(gridLayer[cy + 1][cx]) > 1)
                    heapInsertOrUpdate(seamHeap, gridLayer, cx, cy + 1, &rngState);
                if (cx > 0 && CGSME_POPCOUNT(gridLayer[cy][cx - 1]) > 1)
                    heapInsertOrUpdate(seamHeap, gridLayer, cx - 1, cy, &rngState);
                if (cx + 1 < width && CGSME_POPCOUNT(gridLayer[cy][cx + 1]) > 1)
                    heapInsertOrUpdate(seamHeap, gridLayer, cx + 1, cy, &rngState);
            }
            seamIter++;
        }

        if (seamHeap)
            freeHeap(seamHeap);
    }

    for (uint32_t i = 0; i < length; i++)
    {
        for (uint32_t j = 0; j < width; j++)
        {
            if (CGSME_POPCOUNT(gridLayer[i][j]) > 1)
            {
                if (fulness < 100 && CGSME_NUM_SOCKETS <= 2)
                    gridLayer[i][j] = Empty_Tile;
                else
                    gridLayer[i][j] = CGSME_BIT(CGSME_CTZ(gridLayer[i][j]));
            }
        }
    }

    if (anyOpen != Empty_Tile && CGSME_NUM_SOCKETS <= 2)
    {
        fixupEdges(gridLayer, width, length);
        sealMazeEdges(gridLayer, width, length);
        repairMazePortsInPlace(gridLayer, width, length);

        bool allConnected = true;
        bool postWeldRepaired = false;
        for (int weldPass = 0; weldPass < 3; weldPass++)
        {
            bool usedSuperset = false;
            findConnectedRegionsInPlace(gridLayer, width, length);
            germanWelderExInPlace(gridLayer, width, length, &rngState, &allConnected, &usedSuperset);

            for (uint32_t i = 0; i < length; i++)
            {
                for (uint32_t j = 0; j < width; j++)
                {
                    if (gridLayer[i][j] == CGSME_PACKED_VOID)
                        gridLayer[i][j] = Empty_Tile;
                    else
                    {
                        uint8_t index = (uint8_t)(gridLayer[i][j] & CGSME_TILE_INDEX_MASK);
                        gridLayer[i][j] = indexToMask(index);
                    }
                }
            }

            if (!usedSuperset)
            {
                postWeldRepaired = false;
                break;
            }

            postWeldRepaired = repairMazePortsInPlace(gridLayer, width, length);
            if (!postWeldRepaired)
                break;
        }

        if (!allConnected || postWeldRepaired)
            pruneUnreachableComponentsInPlace(gridLayer, width, length);
    }
}

typedef struct
{
    uint32_t flatA;
    uint8_t dir;
} AriadneEdge3D;

static inline uint8_t portBits6OfTile(uint8_t idx)
{
    cgsme_tile_t bit = CGSME_BIT(idx);
    uint8_t m = 0;
    for (int d = 0; d < 4; d++)
    {
        if (bit & CGSME_HAS_PORT_MASK[d])
            m |= (uint8_t)(1u << d);
    }
    if ((bit & CGSME_VERT_PROP_MASK[CGSME_NUM_TILES][VERT_IDX_DOWN]) == Empty_Tile)
        m |= (uint8_t)(1u << 4); // U (z + 1)
    if ((bit & CGSME_VERT_PROP_MASK[CGSME_NUM_TILES][VERT_IDX_UP]) == Empty_Tile)
        m |= (uint8_t)(1u << 5); // D (z - 1)
    return m;
}

static void expandAriadne3D(
    const uint8_t *p6, uint8_t *vis,
    uint32_t width, uint32_t length, uint32_t height,
    uint32_t startFlat, uint32_t *bfsQ,
    AriadneEdge3D *frontier, uint32_t *frontierCount, uint32_t maxFrontier)
{
    static const int32_t dx6[6]  = {  0,  1,  0, -1,  0,  0 };
    static const int32_t dy6[6]  = { -1,  0,  1,  0,  0,  0 };
    static const int32_t dz6[6]  = {  0,  0,  0,  0,  1, -1 };
    static const uint8_t opp6[6] = {  2,  3,  0,  1,  5,  4 };

    uint32_t head = 0, tail = 0;
    vis[startFlat] = 1;
    bfsQ[tail++] = startFlat;
    uint32_t layerArea = width * length;

    while (head < tail)
    {
        uint32_t a = bfsQ[head++];
        uint32_t z = a / layerArea;
        uint32_t rem = a % layerArea;
        uint32_t y = rem / width;
        uint32_t x = rem % width;
        uint8_t pa = p6[a];

        for (uint8_t d = 0; d < 6; d++)
        {
            int32_t nx = (int32_t)x + dx6[d];
            int32_t ny = (int32_t)y + dy6[d];
            int32_t nz = (int32_t)z + dz6[d];
            if (nx < 0 || ny < 0 || nz < 0 ||
                (uint32_t)nx >= width || (uint32_t)ny >= length || (uint32_t)nz >= height)
                continue;

            uint32_t b = ((uint32_t)nz * length + (uint32_t)ny) * width + (uint32_t)nx;
            uint8_t pb = p6[b];
            if (pb == 0 || vis[b])
                continue;

            if ((pa & (1u << d)) && (pb & (1u << opp6[d])))
            {
                vis[b] = 1;
                bfsQ[tail++] = b;
            }
            else if (*frontierCount < maxFrontier)
            {
                frontier[(*frontierCount)++] = (AriadneEdge3D){ a, d };
            }
        }
    }
}

static void finalize3DPipeTopology(
    cgsme_tile_t ***grid, uint32_t width, uint32_t length, uint32_t height, uint32_t *rng)
{
    static const int32_t dx6[6]  = {  0,  1,  0, -1,  0,  0 };
    static const int32_t dy6[6]  = { -1,  0,  1,  0,  0,  0 };
    static const int32_t dz6[6]  = {  0,  0,  0,  0,  1, -1 };
    static const uint8_t opp6[6] = {  2,  3,  0,  1,  5,  4 };

    uint8_t mask6ToTile[64];
    uint8_t tileToMask6[64];
    for (int m = 0; m < 64; m++)
    {
        mask6ToTile[m] = 0xFF;
        tileToMask6[m] = 0;
    }
    for (int t = 0; t < CGSME_NUM_TILES; t++)
    {
        uint8_t m = portBits6OfTile((uint8_t)t);
        tileToMask6[t] = m;
        if (CGSME_CATEGORY_NATURAL[CGSME_BIT_TO_CATEGORY[t]] && mask6ToTile[m] == 0xFF)
            mask6ToTile[m] = (uint8_t)t;
    }

    uint32_t layerArea = width * length;
    uint32_t total3D = layerArea * height;
    uint8_t *p6 = calloc(total3D, sizeof(uint8_t));
    if (!p6)
        return;

    for (uint32_t z = 0; z < height; z++)
    {
        for (uint32_t y = 0; y < length; y++)
        {
            for (uint32_t x = 0; x < width; x++)
            {
                cgsme_tile_t cell = grid[z][y][x];
                if (cell != Empty_Tile)
                    p6[(z * length + y) * width + x] = tileToMask6[CGSME_CTZ(cell)];
            }
        }
    }

    // strip ports opening into void or out of bounds
    for (uint32_t z = 0; z < height; z++)
    {
        for (uint32_t y = 0; y < length; y++)
        {
            for (uint32_t x = 0; x < width; x++)
            {
                uint32_t a = (z * length + y) * width + x;
                if (p6[a] == 0)
                    continue;
                for (uint8_t d = 0; d < 6; d++)
                {
                    if (!(p6[a] & (1u << d)))
                        continue;
                    int32_t nx = (int32_t)x + dx6[d];
                    int32_t ny = (int32_t)y + dy6[d];
                    int32_t nz = (int32_t)z + dz6[d];
                    if (nx < 0 || ny < 0 || nz < 0 ||
                        (uint32_t)nx >= width || (uint32_t)ny >= length || (uint32_t)nz >= height ||
                        grid[nz][ny][nx] == Empty_Tile)
                    {
                        p6[a] &= (uint8_t)~(1u << d);
                    }
                }
            }
        }
    }

    // make interior 3d edges symmetric
    for (uint32_t z = 0; z < height; z++)
    {
        for (uint32_t y = 0; y < length; y++)
        {
            for (uint32_t x = 0; x < width; x++)
            {
                uint32_t a = (z * length + y) * width + x;
                if (grid[z][y][x] == Empty_Tile)
                    continue;

                static const uint8_t posDirs[3] = { 1, 2, 4 }; // E(+X), S(+Y), U(+Z)
                for (int k = 0; k < 3; k++)
                {
                    uint8_t d = posDirs[k];
                    uint8_t opp = opp6[d];
                    int32_t nx = (int32_t)x + dx6[d];
                    int32_t ny = (int32_t)y + dy6[d];
                    int32_t nz = (int32_t)z + dz6[d];
                    if (nx >= (int32_t)width || ny >= (int32_t)length || nz >= (int32_t)height)
                        continue;
                    if (grid[nz][ny][nx] == Empty_Tile)
                        continue;

                    uint32_t b = ((uint32_t)nz * length + (uint32_t)ny) * width + (uint32_t)nx;
                    uint8_t bitA = (uint8_t)((p6[a] >> d) & 1u);
                    uint8_t bitB = (uint8_t)((p6[b] >> opp) & 1u);
                    if (bitA == bitB)
                        continue;

                    uint8_t openA  = (uint8_t)(p6[a] | (1u << d));
                    uint8_t openB  = (uint8_t)(p6[b] | (1u << opp));
                    uint8_t closeA = (uint8_t)(p6[a] & ~(1u << d));
                    uint8_t closeB = (uint8_t)(p6[b] & ~(1u << opp));

                    if (mask6ToTile[openA] != 0xFF && mask6ToTile[openB] != 0xFF)
                    {
                        p6[a] = openA;
                        p6[b] = openB;
                    }
                    else
                    {
                        p6[a] = closeA;
                        p6[b] = closeB;
                    }
                }
            }
        }
    }

    // if a non-void cell ended up with all 6 ports closed, open 1 valid edge to a neighbor
    for (uint32_t z = 0; z < height; z++)
    {
        for (uint32_t y = 0; y < length; y++)
        {
            for (uint32_t x = 0; x < width; x++)
            {
                uint32_t a = (z * length + y) * width + x;
                if (grid[z][y][x] == Empty_Tile || (p6[a] != 0 && mask6ToTile[p6[a]] != 0xFF))
                    continue;

                for (uint8_t d = 0; d < 6; d++)
                {
                    int32_t nx = (int32_t)x + dx6[d];
                    int32_t ny = (int32_t)y + dy6[d];
                    int32_t nz = (int32_t)z + dz6[d];
                    if (nx < 0 || ny < 0 || nz < 0 ||
                        (uint32_t)nx >= width || (uint32_t)ny >= length || (uint32_t)nz >= height)
                        continue;
                    if (grid[nz][ny][nx] == Empty_Tile)
                        continue;

                    uint32_t b = ((uint32_t)nz * length + (uint32_t)ny) * width + (uint32_t)nx;
                    uint8_t openA = (uint8_t)(1u << d);
                    uint8_t openB = (uint8_t)(p6[b] | (1u << opp6[d]));
                    if (mask6ToTile[openA] != 0xFF && mask6ToTile[openB] != 0xFF)
                    {
                        p6[a] = openA;
                        p6[b] = openB;
                        break;
                    }
                }
            }
        }
    }

    // 3d 6-way ariadne welder
    uint8_t *vis = calloc(total3D, sizeof(uint8_t));
    uint32_t *bfsQ = malloc(total3D * sizeof(uint32_t));
    uint32_t maxFrontier = total3D * 6u;
    AriadneEdge3D *frontier = malloc(maxFrontier * sizeof(AriadneEdge3D));

    if (vis && bfsQ && frontier)
    {
        uint32_t frontierCount = 0;
        for (uint32_t start = 0; start < total3D; start++)
        {
            if (p6[start] == 0 || vis[start])
                continue;

            expandAriadne3D(p6, vis, width, length, height, start, bfsQ, frontier, &frontierCount, maxFrontier);

            while (frontierCount > 0)
            {
                uint32_t pick = nextRandom(rng) % frontierCount;
                AriadneEdge3D e = frontier[pick];
                frontier[pick] = frontier[--frontierCount];

                uint32_t z = e.flatA / layerArea;
                uint32_t rem = e.flatA % layerArea;
                uint32_t y = rem / width;
                uint32_t x = rem % width;

                int32_t nx = (int32_t)x + dx6[e.dir];
                int32_t ny = (int32_t)y + dy6[e.dir];
                int32_t nz = (int32_t)z + dz6[e.dir];
                if (nx < 0 || ny < 0 || nz < 0 ||
                    (uint32_t)nx >= width || (uint32_t)ny >= length || (uint32_t)nz >= height)
                    continue;

                uint32_t b = ((uint32_t)nz * length + (uint32_t)ny) * width + (uint32_t)nx;
                if (p6[b] == 0 || vis[b])
                    continue;

                uint8_t opp = opp6[e.dir];
                uint8_t openA = (uint8_t)(p6[e.flatA] | (1u << e.dir));
                uint8_t openB = (uint8_t)(p6[b] | (1u << opp));
                if (mask6ToTile[openA] == 0xFF || mask6ToTile[openB] == 0xFF)
                    continue;

                p6[e.flatA] = openA;
                p6[b]       = openB;
                expandAriadne3D(p6, vis, width, length, height, b, bfsQ, frontier, &frontierCount, maxFrontier);
            }
        }

        // keep only largest 3d connected component
        int32_t *compId = malloc(total3D * sizeof(int32_t));
        if (compId)
        {
            for (uint32_t i = 0; i < total3D; i++)
                compId[i] = -1;

            int32_t numComps = 0, bestComp = -1;
            uint32_t bestSize = 0;

            for (uint32_t s = 0; s < total3D; s++)
            {
                if (p6[s] == 0 || mask6ToTile[p6[s]] == 0xFF || compId[s] != -1)
                    continue;
                int32_t cid = numComps++;
                uint32_t head = 0, tail = 0, sz = 0;
                compId[s] = cid;
                bfsQ[tail++] = s;

                while (head < tail)
                {
                    uint32_t a = bfsQ[head++];
                    sz++;
                    uint32_t z = a / layerArea;
                    uint32_t rem = a % layerArea;
                    uint32_t y = rem / width;
                    uint32_t x = rem % width;
                    uint8_t pa = p6[a];

                    for (uint8_t d = 0; d < 6; d++)
                    {
                        if (!(pa & (1u << d)))
                            continue;
                        int32_t nx = (int32_t)x + dx6[d];
                        int32_t ny = (int32_t)y + dy6[d];
                        int32_t nz = (int32_t)z + dz6[d];
                        if (nx < 0 || ny < 0 || nz < 0 ||
                            (uint32_t)nx >= width || (uint32_t)ny >= length || (uint32_t)nz >= height)
                            continue;
                        uint32_t b = ((uint32_t)nz * length + (uint32_t)ny) * width + (uint32_t)nx;
                        if (compId[b] != -1)
                            continue;
                        if (p6[b] != 0 && (p6[b] & (1u << opp6[d])))
                        {
                            compId[b] = cid;
                            bfsQ[tail++] = b;
                        }
                    }
                }
                if (sz > bestSize)
                {
                    bestSize = sz;
                    bestComp = cid;
                }
            }

            if (numComps > 1 && bestComp != -1)
            {
                for (uint32_t i = 0; i < total3D; i++)
                    if (compId[i] != bestComp)
                        p6[i] = 0;
            }
            free(compId);
        }
    }

    free(vis);
    free(bfsQ);
    free(frontier);

    for (uint32_t z = 0; z < height; z++)
    {
        for (uint32_t y = 0; y < length; y++)
        {
            for (uint32_t x = 0; x < width; x++)
            {
                uint8_t m = p6[(z * length + y) * width + x];
                grid[z][y][x] = (m != 0 && mask6ToTile[m] != 0xFF) ? CGSME_BIT(mask6ToTile[m]) : Empty_Tile;
            }
        }
    }

    free(p6);
}

#ifdef CGSME_HAS_C11_ATOMICS
typedef struct
{
    atomic_uint arrived;
    atomic_uint sense;
    uint32_t totalWorkers;
} CheckerBarrier;

static inline void checkerBarrierInit(CheckerBarrier *b, uint32_t totalWorkers)
{
    atomic_init(&b->arrived, 0u);
    atomic_init(&b->sense, 0u);
    b->totalWorkers = totalWorkers;
}

static inline void checkerBarrierWait(CheckerBarrier *b, uint32_t *localSense)
{
    if (b->totalWorkers <= 1u)
        return;
    *localSense ^= 1u;
    if (atomic_fetch_add_explicit(&b->arrived, 1u, memory_order_acq_rel) + 1u == b->totalWorkers)
    {
        atomic_store_explicit(&b->arrived, 0u, memory_order_relaxed);
        atomic_store_explicit(&b->sense, *localSense, memory_order_release);
    }
    else
    {
        while (atomic_load_explicit(&b->sense, memory_order_acquire) != *localSense)
            thrd_yield();
    }
}
#endif

typedef struct
{
    cgsme_tile_t ***grid;
    uint32_t width, length, height;
    uint32_t seed, fulness;
    uint32_t workerId;
    uint32_t numWorkers;
    uint32_t maxChunkW, maxChunkL;
    bool hasNatVert;
    CheckerChunkTask *phaseTasks[4];
    uint32_t phaseCounts[4];
#ifdef CGSME_HAS_C11_ATOMICS
    CheckerBarrier *barrier;
#endif
} CheckerPoolWorkerArgs;

static int runCheckerPoolWorker(void *argPtr)
{
    CheckerPoolWorkerArgs *w = (CheckerPoolWorkerArgs *)argPtr;
    ChunkHeap3D *heap = initChunkHeap3D(w->maxChunkW, w->maxChunkL, w->height);
    if (!heap)
        return 1;

#ifdef CGSME_HAS_C11_ATOMICS
    uint32_t localSense = 0u;
#endif

    // 4-color checkerboard passes (0=red, 1=blue, 2=green, 3=yellow)
    for (int phase = 0; phase < 4; phase++)
    {
        uint32_t count = w->phaseCounts[phase];
        const CheckerChunkTask *tasks = w->phaseTasks[phase];
        for (uint32_t idx = w->workerId; idx < count; idx += w->numWorkers)
        {
            const CheckerChunkTask *t = &tasks[idx];
            solveColumnChunk3D(
                w->grid, w->width, w->length, w->height,
                t->x0, t->y0, t->x1, t->y1,
                t->chunkSeed, w->fulness, heap);
        }
#ifdef CGSME_HAS_C11_ATOMICS
        if (w->barrier)
            checkerBarrierWait(w->barrier, &localSense);
#endif
    }

    freeChunkHeap3D(heap);

    // layer seam cleanup + welding for 2.5d layers
    if (!w->hasNatVert)
    {
        for (uint32_t z = w->workerId; z < w->height; z += w->numWorkers)
        {
            uint32_t layerSeed = w->seed ^ ((z + 1u) * 0xC2B2AE35u);
            nextRandom(&layerSeed);
            finalizeLayerSeamsAndWeld(w->grid, w->width, w->length, w->height, z, layerSeed, w->fulness);
        }
    }

    return 0;
}

cgsme_tile_t ***generateGrid(uint32_t width, uint32_t length, uint32_t height, uint32_t seed, uint32_t fulness)
{
    CGSME_PROFILE_FUNC();

    if (width < 4 || length < 4 || height < 1)
    {
        return NULL;
    }

#ifdef cgsme_DEBUG
    uint64_t __cgsme_grid_start = cgsme_now_us();
    uint64_t __cgsme_grid_start_cycles = cgsme_now_cycles();
#endif

    cgsme_tile_t ***grid = malloc(sizeof(cgsme_tile_t **) * height);
    cgsme_tile_t **all_rows = malloc(height * length * sizeof(cgsme_tile_t *));
    cgsme_tile_t *all_data = calloc(height * length * width, sizeof(cgsme_tile_t));

    for (uint32_t i = 0; i < height; i++)
    {
        grid[i] = &all_rows[i * length];
        for (uint32_t j = 0; j < length; j++)
        {
            grid[i][j] = &all_data[(i * length + j) * width];
        }
    }

    // mask + void clamping + area quotas
    runArchitect(grid, width, length, height, fulness, seed);

    // split into 4-color checkerboard column chunks for 2-socket mazes/pipes
    // multi-socket rulesets (>2 sockets) run per-layer or full-volume so gradients dont break on chunk seams
    cgsme_tile_t natMask = Empty_Tile;
    bool hasNatVert = false;
    for (int t = 0; t < CGSME_NUM_TILES; t++)
    {
        if (CGSME_CATEGORY_NATURAL[CGSME_BIT_TO_CATEGORY[t]])
            natMask |= CGSME_BIT(t);
    }
    for (int t = 0; t < CGSME_NUM_TILES && !hasNatVert; t++)
    {
        if (CGSME_CATEGORY_NATURAL[CGSME_BIT_TO_CATEGORY[t]])
        {
            if ((CGSME_VERT_PROP_MASK[t][0] & natMask) != natMask ||
                (CGSME_VERT_PROP_MASK[t][1] & natMask) != natMask)
                hasNatVert = true;
        }
    }

    if (CGSME_NUM_SOCKETS > 2)
    {
        if (hasNatVert && height > 1)
        {
            ChunkHeap3D *heap3D = initChunkHeap3D(width, length, height);
            if (heap3D)
            {
                solveColumnChunk3D(grid, width, length, height, 0, 0, width - 1, length - 1, seed, fulness, heap3D);
                freeChunkHeap3D(heap3D);
            }
            for (uint32_t z = 0; z < height; z++)
                for (uint32_t y = 0; y < length; y++)
                    for (uint32_t x = 0; x < width; x++)
                        if (CGSME_POPCOUNT(grid[z][y][x]) > 1)
                            grid[z][y][x] = CGSME_BIT(CGSME_CTZ(grid[z][y][x]));
        }
        else
        {
            thrd_t *threads = malloc(sizeof(thrd_t) * height);
            layerGenerationArgs *args = malloc(sizeof(layerGenerationArgs) * height);
            int32_t centerX = (int32_t)(width / 2);
            int32_t centerY = (int32_t)(length / 2);
            uint32_t s = seed;
            for (uint32_t i = 0; i < height; i++)
            {
                args[i].gridLayer = grid[i];
                args[i].width = width;
                args[i].length = length;
                args[i].startX = centerX;
                args[i].startY = centerY;
                args[i].endX = centerX;
                args[i].endY = centerY;
                args[i].seed = nextRandom(&s);
                args[i].fulness = (uint8_t)fulness;
                args[i].layerIndex = (uint8_t)i;
                thrd_create(&threads[i], generateLayerThread, (void *)&args[i]);
            }
            for (uint32_t i = 0; i < height; i++)
                thrd_join(threads[i], NULL);
            free((void *)threads);
            free((void *)args);
        }

#ifdef cgsme_DEBUG
        uint64_t __cgsme_grid_end = cgsme_now_us();
        uint64_t __cgsme_grid_end_cycles = cgsme_now_cycles();
        cgsme_profile_record("generateGrid", (unsigned long long)(__cgsme_grid_end - __cgsme_grid_start), (unsigned long long)(__cgsme_grid_end_cycles - __cgsme_grid_start_cycles));
        cgsme_profile_set_runinfo(height, width, length, seed, fulness);
#endif
        return grid;
    }

    uint32_t baseAreaSize = (CGSME_NUM_ANCHORS > 0 && CGSME_ANCHOR_AREA_SIZE[0] >= 8) ? CGSME_ANCHOR_AREA_SIZE[0] : 20u;
    uint32_t chunkSize = baseAreaSize;
    if (width >= baseAreaSize * 4u && length >= baseAreaSize * 4u)
        chunkSize = baseAreaSize * 2u;
    if (chunkSize > 64u)
        chunkSize = 64u;
    if (width < chunkSize * 2u || length < chunkSize * 2u)
    {
        uint32_t minDim = (width < length) ? width : length;
        uint32_t halfDim = minDim / 2u;
        if (halfDim >= 6u && halfDim < chunkSize)
            chunkSize = halfDim;
    }

    uint32_t chunksX = (width + chunkSize - 1u) / chunkSize;
    uint32_t chunksY = (length + chunkSize - 1u) / chunkSize;
    uint32_t totalChunks = chunksX * chunksY;

    CheckerChunkTask *phaseTasks[4] = { NULL, NULL, NULL, NULL };
    uint32_t phaseCounts[4] = { 0, 0, 0, 0 };
    CheckerChunkTask *allTasks = malloc(sizeof(CheckerChunkTask) * totalChunks * 4u);
    for (int p = 0; p < 4; p++)
        phaseTasks[p] = &allTasks[(size_t)p * totalChunks];

    for (uint32_t cy = 0; cy < chunksY; cy++)
    {
        uint32_t y0 = cy * chunkSize;
        uint32_t y1 = (y0 + chunkSize < length) ? (y0 + chunkSize - 1u) : (length - 1u);
        for (uint32_t cx = 0; cx < chunksX; cx++)
        {
            uint32_t x0 = cx * chunkSize;
            uint32_t x1 = (x0 + chunkSize < width) ? (x0 + chunkSize - 1u) : (width - 1u);
            uint32_t phase = (cx & 1u) | ((cy & 1u) << 1u);

            uint32_t cSeed = seed ^ ((cx + 1u) * 0x9E3779B9u) ^ ((cy + 1u) * 0x85EBCA6Bu);
            nextRandom(&cSeed);

            phaseTasks[phase][phaseCounts[phase]++] = (CheckerChunkTask){
                cx, cy, x0, y0, x1, y1, cSeed
            };
        }
    }

    uint32_t maxPhaseChunks = phaseCounts[0];
    for (int p = 1; p < 4; p++)
        if (phaseCounts[p] > maxPhaseChunks)
            maxPhaseChunks = phaseCounts[p];

    uint32_t nThreads = (maxPhaseChunks > height) ? maxPhaseChunks : height;
    if (nThreads > 8u)
        nThreads = 8u;
    if ((uint64_t)width * length * height < 8192ULL)
        nThreads = 1u;

#ifdef CGSME_HAS_C11_ATOMICS
    CheckerBarrier barrier;
    checkerBarrierInit(&barrier, nThreads);

    thrd_t workers[8];
    CheckerPoolWorkerArgs poolArgs[8];

    for (uint32_t w = 0; w < nThreads; w++)
    {
        poolArgs[w].grid = grid;
        poolArgs[w].width = width;
        poolArgs[w].length = length;
        poolArgs[w].height = height;
        poolArgs[w].seed = seed;
        poolArgs[w].fulness = fulness;
        poolArgs[w].workerId = w;
        poolArgs[w].numWorkers = nThreads;
        poolArgs[w].maxChunkW = chunkSize;
        poolArgs[w].maxChunkL = chunkSize;
        poolArgs[w].hasNatVert = hasNatVert;
        for (int p = 0; p < 4; p++)
        {
            poolArgs[w].phaseTasks[p] = phaseTasks[p];
            poolArgs[w].phaseCounts[p] = phaseCounts[p];
        }
        poolArgs[w].barrier = &barrier;
    }

    for (uint32_t w = 1; w < nThreads; w++)
        thrd_create(&workers[w], runCheckerPoolWorker, &poolArgs[w]);

    // main thread runs as worker 0
    runCheckerPoolWorker(&poolArgs[0]);

    for (uint32_t w = 1; w < nThreads; w++)
        thrd_join(workers[w], NULL);
#else
    CheckerPoolWorkerArgs singleArg;
    singleArg.grid = grid;
    singleArg.width = width;
    singleArg.length = length;
    singleArg.height = height;
    singleArg.seed = seed;
    singleArg.fulness = fulness;
    singleArg.workerId = 0u;
    singleArg.numWorkers = 1u;
    singleArg.maxChunkW = chunkSize;
    singleArg.maxChunkL = chunkSize;
    singleArg.hasNatVert = hasNatVert;
    for (int p = 0; p < 4; p++)
    {
        singleArg.phaseTasks[p] = phaseTasks[p];
        singleArg.phaseCounts[p] = phaseCounts[p];
    }
    runCheckerPoolWorker(&singleArg);
#endif

    free(allTasks);

    if (hasNatVert)
    {
        for (uint32_t z = 0; z < height; z++)
            for (uint32_t y = 0; y < length; y++)
                for (uint32_t x = 0; x < width; x++)
                    if (CGSME_POPCOUNT(grid[z][y][x]) > 1)
                        grid[z][y][x] = CGSME_BIT(CGSME_CTZ(grid[z][y][x]));

        uint32_t weldRng = seed ^ 0xA5A5F0F0u;
        nextRandom(&weldRng);
        finalize3DPipeTopology(grid, width, length, height, &weldRng);
    }

    // double check vertical connector alignment between z and z+1
    if (height > 1 && CGSME_NUM_ANCHORS > 0)
    {
        for (uint32_t z = 0; z + 1 < height; z++)
        {
            for (uint32_t y = 1; y + 1 < length; y++)
            {
                for (uint32_t x = 1; x + 1 < width; x++)
                {
                    cgsme_tile_t loCell = grid[z][y][x];
                    if (loCell == Empty_Tile || CGSME_POPCOUNT(loCell) != 1)
                        continue;

                    uint8_t loIdx = (uint8_t)CGSME_CTZ(loCell);
                    cgsme_tile_t reqUp = CGSME_VERT_PROP_MASK[loIdx][VERT_IDX_UP];
                    if (reqUp == All_Possible_State)
                        continue;

                    cgsme_tile_t hiCell = grid[z + 1][y][x];
                    if (hiCell != Empty_Tile && (hiCell & reqUp) != Empty_Tile)
                        continue;

                    // if layer z+1 lost the receiver above a stair, downgrade it to its normal horizontal tile
                    uint8_t fallbackIdx = CGSME_OPEN_LUT[loIdx][0];
                    if (fallbackIdx != loIdx && (CGSME_VERT_PROP_MASK[fallbackIdx][VERT_IDX_UP] & hiCell) != Empty_Tile)
                        grid[z][y][x] = CGSME_BIT(fallbackIdx);
                    else if (hiCell == Empty_Tile && (CGSME_VERT_PROP_MASK[CGSME_NUM_TILES][VERT_IDX_DOWN] & CGSME_BIT(fallbackIdx)) != Empty_Tile)
                        grid[z][y][x] = CGSME_BIT(fallbackIdx);
                }
            }
        }
    }

#ifdef cgsme_DEBUG
    uint64_t __cgsme_grid_end = cgsme_now_us();
    uint64_t __cgsme_grid_end_cycles = cgsme_now_cycles();
    cgsme_profile_record("generateGrid", (unsigned long long)(__cgsme_grid_end - __cgsme_grid_start), (unsigned long long)(__cgsme_grid_end_cycles - __cgsme_grid_start_cycles));
    cgsme_profile_set_runinfo(height, width, length, seed, fulness);
#endif

    return grid;
}


void freeGrid(cgsme_tile_t ***grid, uint32_t width, uint32_t length, uint32_t height)
{
    CGSME_PROFILE_FUNC();
    if (grid != NULL)
    {
        // grid[0] contains the pointer to all_rows
        if (grid[0] != NULL)
        {
            // grid[0][0] contains the pointer to all_data
            free((void *)grid[0][0]);
        }

        free((void *)grid[0]); // free all_rows
        free((void *)grid);    // free the top level grid pointer
    }
}

