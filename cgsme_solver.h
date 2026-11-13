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

/// Collapse a tile bitmask down to a single selected variant.
///
/// Parameters:
///     tile  - pointer to a tile bitmask where each set bit represents a valid tile
///             variant. On return *tile will contain exactly one set bit representing
///             the chosen variant. If *tile == 0 the function does nothing.
///     rates - pointer to an array of `CGSME_NUM_CATEGORIES` floats representing spawn
///             weights for each tile category. Each valid bit in the bitmask is
///             mapped to a category via `CGSME_BIT_TO_CATEGORY` and selection is weighted
///             by `rates[category]`.
///     rng   - per-thread RNG state.
///
/// Behavior:
///     - Weighted pick among the set bits. If the summed weight is ~0 it falls back
///       to a uniform pick among the set bits.
///     - Only visits set bits (t &= t - 1), so cost is O(popcount) not O(CGSME_NUM_TILES).
void collapseTile(cgsme_tile_t *tile, float *rates, uint32_t *rng);

/// Update neighbors' possible states by constraining them to match the tile at (x,y).
///
/// Behavior / Notes:
///     - Looks up the 4 neighbor masks in CGSME_PROP_MASK. Row CGSME_NUM_TILES is the
///       Empty_Tile (void) row. Uncollapsed cells apply no restriction.
///     - neighbor &= mask for every in-bounds, still-uncollapsed neighbor.
///     - A neighbor that drops to 0 (contradiction) is revived to All_Possible_State.
///     - If heap != NULL, changed neighbors are pushed / re-scored in the heap.
void updateNeighbours(cgsme_tile_t **gridLayer, uint32_t width, uint32_t length, uint32_t x, uint32_t y, MinHeap *heap, uint32_t *rng);

/// Recalculate category spawn rates using a Gaussian model and connector boost.
///
/// Parameters:
///     rates             - caller-provided array of `CGSME_NUM_CATEGORIES` floats to fill.
///     current_collapsed - number of tiles already collapsed.
///     target_collapsed  - desired number of collapsed tiles.
///
/// Behavior:
///     - The Gaussian peak slides from curve_pos 0 to the largest curve_pos of any
///       natural_spawn category as progress goes 0 -> 1.
///     - Categories with connector_boost get an extra boost that fades out with progress.
///     - Categories with natural_spawn == false always get 0.
void update_spawnrates(float rates[], int current_collapsed, int target_collapsed);

/// @brief Check if a tile is required (any collapsed neighbor opens toward it).
bool isTileRequired(cgsme_tile_t **grid, uint32_t width, uint32_t length, uint32_t x, uint32_t y);

/// @brief Find the next seed location when the heap runs empty.
bool findBestSeedLocation(cgsme_tile_t **grid, uint32_t width, uint32_t length, uint32_t *outX, uint32_t *outY, uint32_t *rng);

/// @brief Calculate the heap score for a tile (entropy + tiny tie-break noise).
float calculateScore(cgsme_tile_t **grid, uint32_t x, uint32_t y, uint32_t *rng);

#endif // CGSME_SOLVER_H
