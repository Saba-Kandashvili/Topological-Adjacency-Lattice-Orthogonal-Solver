#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <stdbool.h>
#include <stdio.h>
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

/// Generate a 3D grid using Wave Function Collapse (WFC).
///
/// Parameters:
///     width   - number of columns per layer.
///     length  - number of rows per layer.
///     height  - number of layers.
///     seed    - RNG seed used to initialize per-thread RNGs.
///     fulness - target percentage (0-100) of tiles to collapse; a minimum
///               collapsed count is enforced by the implementation.
///
/// Returns:
///     Pointer to a newly-allocated 3D grid. The return value is an array of
///     `height` pointers to layers; each layer is an array of `length` row
///     pointers and each row contains `width` `cgsme_tile_t` entries. The caller
///     owns the returned memory and must free it with `freeGrid(grid, width,
///     length, height)`.
///
/// Behavior / Notes:
///     - Allocates memory with `malloc` for the top-level array, each layer,
///       and each row, then spawns one thread per layer via `thrd_create` to
///       run `generateLayerThread`.
///     - The function currently does not perform robust malloc error handling;
///       partial allocation may lead to undefined behavior if memory allocation
///       fails.
cgsme_tile_t ***generateGrid(uint32_t width, uint32_t length, uint32_t height, uint32_t seed, uint32_t fulness);

/// Free a grid previously returned by `generateGrid`.
///
/// Parameters:
///     grid   - pointer returned by `generateGrid`.
///     width  - width used when allocating.
///     length - length used when allocating.
///     height - height used when allocating.
///
/// Behavior / Notes:
///     - Frees row arrays, then layer arrays, then the top-level grid pointer.
///     - Expects the same dimensions used to allocate; passing a NULL pointer
///       or mismatched dimensions is undefined (the function does not check
///       for NULL in the current implementation).
void freeGrid(cgsme_tile_t ***grid, uint32_t width, uint32_t length, uint32_t height);
// Helper executed on a worker thread to generate a single layer.
// Parameters:
//     args - pointer to a `layerGenerationArgs` struct (defined in generator.h).
// Returns:
//     thrd-compatible int (thread return code). Populates the provided
//     `gridLayer` inside `args` and uses a per-thread RNG seeded from
//     `args->seed`.
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


// ARCHITECT LOGIC (pre-seeding)
// runs purely on the main thread before any threads are spawned
void runArchitect(cgsme_tile_t ***grid, uint32_t width, uint32_t length, uint32_t height, uint32_t fulness, uint32_t seed)
{
    CGSME_PROFILE_FUNC();

    // rand that produces identical numbes on both windows and linux
    uint32_t rngState = seed;

    // because of calloc the grid is already initialized to Empty_Tile (0)

    // generate Mask (Ridged Noise + Sanitize + Rescue)
    if (fulness < 100)
    {
        // the generator will write All_Possible_State
        // to valid locations. everything else stays 0
        generateRidgedMask(grid, width, length, height, fulness, seed);
    }
    else
    {
        for (uint32_t z = 0; z < height; z++)
            for (uint32_t y = 0; y < length; y++)
                for (uint32_t x = 0; x < width; x++)
                    grid[z][y][x] = All_Possible_State;
    }

    // place vertical anchors (stairs) — defined in tileset.json "architect.vertical_anchors"
    // lower tile goes on layer z, upper tile goes on layer z + 1 at the same (x, y)
    for (uint32_t z = 0; z < height - 1; z++)
    {
        for (int a = 0; a < CGSME_NUM_ANCHORS; a++)
        {
            const cgsme_tile_t lowerTile = CGSME_BIT(CGSME_ANCHOR_LOWER[a]);
            const cgsme_tile_t upperTile = CGSME_BIT(CGSME_ANCHOR_UPPER[a]);

            int anchorsPerLayer = (int)((width * length) / CGSME_ANCHOR_DIVISOR[a]);
            if (anchorsPerLayer < (int)CGSME_ANCHOR_MIN[a])
                anchorsPerLayer = (int)CGSME_ANCHOR_MIN[a];

            int placedCount = 0;
            int attempts = 0;
            int maxAttempts = anchorsPerLayer * 20;

            while (placedCount < anchorsPerLayer && attempts < maxAttempts)
            {
                attempts++;
                int x = nextRandom(&rngState) % width;
                int y = nextRandom(&rngState) % length;

                // bounds
                if (x < 1 || y < 1 || x >= width - 1 || y >= length - 1)
                    continue;

                // VOID CHECK must be on valid land
                if (grid[z][y][x] == Empty_Tile)
                    continue;

                // occupancy
                if (grid[z][y][x] != All_Possible_State)
                    continue;

                // anti-stacking
                if (z > 0 && grid[z - 1][y][x] == lowerTile)
                    continue;

                grid[z][y][x] = lowerTile;     // stairs up
                grid[z + 1][y][x] = upperTile; // receiver hole
                placedCount++;
            }
        }
    }
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

    // WEIGHTS CONFIGURATION
    float current_spawnrates[CGSME_NUM_CATEGORIES];
    if (fulness < 100)
    {
        // MASK MODE: fixed weights from tileset.json "mask_weight"
        for (int i = 0; i < CGSME_NUM_CATEGORIES; ++i)
            current_spawnrates[i] = CGSME_CATEGORY_MASK_WEIGHT[i];
    }
    else
    {
        // OCEAN MODE: uniform start over the naturally spawning categories only
        int naturalCount = 0;
        for (int i = 0; i < CGSME_NUM_CATEGORIES; ++i)
            if (CGSME_CATEGORY_NATURAL[i])
                naturalCount++;

        for (int i = 0; i < CGSME_NUM_CATEGORIES; ++i)
            current_spawnrates[i] = (CGSME_CATEGORY_NATURAL[i] && naturalCount > 0) ? (1.0f / (float)naturalCount) : 0.0f;
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
            if (gridLayer[i][j] == Empty_Tile)
            {
                // Mask Void: tell neighbors "I am a wall"
                updateNeighbours(gridLayer, width, length, j, i, heap, &rngState);
            }
            else if (CGSME_POPCOUNT(gridLayer[i][j]) == 1)
            {
                // preplaced stairs propagate constraints
                valid_collapsed_count++;
                updateNeighbours(gridLayer, width, length, j, i, heap, &rngState);
            }
        }
    }

    // SEED CENTER (if valid)
    if (gridLayer[startY][startX] != Empty_Tile && CGSME_POPCOUNT(gridLayer[startY][startX]) > 1)
    {
        gridLayer[startY][startX] = CGSME_SEED_TILE_MASK;
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

        // only use dynamic pacing if NOT in mask mode
        if (fulness >= 100 && (iter % 10 == 0 || valid_collapsed_count < 50))
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

                // force seed a tile type (seed tile from tileset.json is flexible)
                if (CGSME_POPCOUNT(gridLayer[cy][cx]) > 1)
                {
                    gridLayer[cy][cx] = CGSME_SEED_TILE_MASK;
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
            }
            else
            {
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

        // VOID LOGIC (only for ocean mode)
        // if hit target count in nonmasked mode start deleting unnecessary tiles
        if (fulness >= 100 && valid_collapsed_count >= target_collapsed_count)
        {
            if (!isTileRequired(gridLayer, width, length, cx, cy))
            {
                gridLayer[cy][cx] = Empty_Tile;
                updateNeighbours(gridLayer, width, length, cx, cy, heap, &rngState);
                valid_collapsed_count--; // adjust count
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

    sealMazeEdges(gridLayer, width, length);
    fixupEdges(gridLayer, width, length);
    findConnectedRegionsInPlace(gridLayer, width, length);
    germanWelderInPlace(gridLayer, width, length, &rngState);

    // free memory
    freeHeap(heap);

    // unpack Regions
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

    return 0;
}


cgsme_tile_t ***generateGrid(uint32_t width, uint32_t length, uint32_t height, uint32_t seed, uint32_t fulness)
{
    CGSME_PROFILE_FUNC();

    // SAFETY CHECK
    // WFC cannot mathematically operate on maps smaller than 3x3 (needs center + neighbors)
    // german welder needs space to exist
    if (width < 4 || length < 4 || height < 1)
    {
        return NULL; // fail instead of crashing
    }

    /* overall generator instrumentation */
#ifdef cgsme_DEBUG
    uint64_t __cgsme_grid_start = cgsme_now_us();
    uint64_t __cgsme_grid_start_cycles = cgsme_now_cycles();
#endif

    // top level pointer
    cgsme_tile_t ***grid = malloc(sizeof(cgsme_tile_t **) * height);

    // mid level pointer
    cgsme_tile_t **all_rows = malloc(height * length * sizeof(cgsme_tile_t *));

    // actual data all at once (all 0s)
    cgsme_tile_t *all_data = calloc(height * length * width, sizeof(cgsme_tile_t));

    // stich so it can be indexed as grid[layer][row][col]
    for (uint32_t i = 0; i < height; i++)
    {
        grid[i] = &all_rows[i * length]; // point to the start of this layer
        for (uint32_t j = 0; j < length; j++)
        {
            // point to the start of this row
            grid[i][j] = &all_data[(i * length + j) * width];
        }
    }

    // ARCHITECT PHASE
    runArchitect(grid, width, length, height, fulness, seed);

    // LAYER GENERATION PHASE (MULTI THREADING)
    thrd_t *threads = malloc(sizeof(thrd_t) * height);
    layerGenerationArgs *args = malloc(sizeof(layerGenerationArgs) * height);

    // standard start point is center
    int32_t centerX = width / 2;
    int32_t centerY = length / 2;

    for (uint32_t i = 0; i < height; i++)
    {
        // esvery layer attempts to start seeding from the center (and the Architect seeds)

        args[i].gridLayer = grid[i];
        args[i].width = width;
        args[i].length = length;

        args[i].startX = centerX;
        args[i].startY = centerY;

        args[i].endX = centerX;
        args[i].endY = centerY;

        args[i].seed = nextRandom(&seed);
        args[i].fulness = fulness;
        args[i].layerIndex = i;

        thrd_create(&threads[i], generateLayerThread, (void *)&args[i]);
    }

    // wait for threads
    for (uint32_t i = 0; i < height; i++)
        thrd_join(threads[i], NULL);

    free((void *)threads);
    free((void *)args);

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

