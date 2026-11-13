#include "cgsme_solver.h"
#include "cgsme_debug.h"
#include "cgsme_utils.h"
#include <math.h>

// returns true if any collapsed neighbor has an open connection pointing to this tile
bool isTileRequired(cgsme_tile_t **grid, uint32_t width, uint32_t length, uint32_t x, uint32_t y)
{
	CGSME_PROFILE_FUNC();
	// check NORTH neighbor (y-1). Does it open SOUTH toward us?
	if (y > 0 && CGSME_POPCOUNT(grid[y - 1][x]) == 1)
	{
		if (grid[y - 1][x] & CGSME_HAS_PORT_MASK[DIR_IDX_S])
			return true;
	}

	// check SOUTH neighbor (y+1). Does it open NORTH toward us?
	if (y < length - 1 && CGSME_POPCOUNT(grid[y + 1][x]) == 1)
	{
		if (grid[y + 1][x] & CGSME_HAS_PORT_MASK[DIR_IDX_N])
			return true;
	}

	// check EAST neighbor (x+1). Does it open WEST toward us?
	if (x < width - 1 && CGSME_POPCOUNT(grid[y][x + 1]) == 1)
	{
		if (grid[y][x + 1] & CGSME_HAS_PORT_MASK[DIR_IDX_W])
			return true;
	}

	// check WEST neighbor (x-1). Does it open EAST toward us?
	if (x > 0 && CGSME_POPCOUNT(grid[y][x - 1]) == 1)
	{
		if (grid[y][x - 1] & CGSME_HAS_PORT_MASK[DIR_IDX_E])
			return true;
	}

	return false;
}

// picks ONE set bit out of `options`, weighted by the category rates
// iterates only over set bits: t &= (t - 1) clears the lowest set bit each step,
// CGSME_CTZ(t) gives the index of the current lowest set bit
static cgsme_tile_t pickWeightedTile(cgsme_tile_t options, const float *rates, uint32_t *rng)
{
	// 1. total weight of the valid options only
	float total_weight = 0.0f;
	for (cgsme_tile_t t = options; t != 0; t &= (t - 1))
		total_weight += rates[CGSME_BIT_TO_CATEGORY[CGSME_CTZ(t)]];

	// EDGE CASE: all valid options have ~0 weight -> uniform random among set bits
	if (total_weight <= 0.0001f)
	{
		uint32_t r = nextRandom(rng) % (uint32_t)CGSME_POPCOUNT(options);
		cgsme_tile_t t = options;
		for (uint32_t k = 0; k < r; k++)
			t &= (t - 1); // drop the r lowest set bits
		return CGSME_BIT(CGSME_CTZ(t));
	}

	// 2. pick a random value within the total weight range (2^32 = 4294967296)
	float random_01 = (float)nextRandom(rng) / 4294967296.0f;
	float random_val = random_01 * total_weight;

	// 3. find the winner
	for (cgsme_tile_t t = options; t != 0; t &= (t - 1))
	{
		int i = CGSME_CTZ(t);
		random_val -= rates[CGSME_BIT_TO_CATEGORY[i]];
		if (random_val <= 0)
			return CGSME_BIT(i);
	}

	// fallback: float rounding finished the loop without a pick -> take the highest set bit
	cgsme_tile_t last = options;
	while (last & (last - 1))
		last &= (last - 1);
	return last;
}

// collapseTile with spawnrates (smooth gaussian model that changes over time)
void collapseTile(cgsme_tile_t *tile, float *rates, uint32_t *rng)
{
#ifdef cgsme_DEBUG
	uint64_t __cgsme_collapse_start = cgsme_now_us();
	uint64_t __cgsme_collapse_start_cycles = cgsme_now_cycles();
#endif
	if (*tile == Empty_Tile)
		return;

	*tile = pickWeightedTile(*tile, rates, rng);

#ifdef cgsme_DEBUG
	if (cgsme_get_enabled())
	{
		uint64_t __cgsme_collapse_end = cgsme_now_us();
		uint64_t __cgsme_collapse_end_cycles = cgsme_now_cycles();
		cgsme_profile_record("collapseTile", (unsigned long long)(__cgsme_collapse_end - __cgsme_collapse_start),
							 (unsigned long long)(__cgsme_collapse_end_cycles - __cgsme_collapse_start_cycles));
	}
#endif
}

void updateNeighbours(cgsme_tile_t **gridLayer, uint32_t width, uint32_t length, uint32_t x, uint32_t y, MinHeap *heap, uint32_t *rng)
{
#ifdef cgsme_DEBUG
	uint64_t __cgsme_neigh_start = cgsme_now_us();
	uint64_t __cgsme_neigh_start_cycles = cgsme_now_cycles();
#endif

	cgsme_tile_t cell = gridLayer[y][x];
	cgsme_tile_t northMask, eastMask, southMask, westMask;

	if (cell == Empty_Tile || CGSME_POPCOUNT(cell) == 1)
	{
		// row CGSME_NUM_TILES of CGSME_PROP_MASK is the Empty_Tile (void) row
		uint32_t lut = (cell == Empty_Tile) ? CGSME_NUM_TILES : (uint32_t)CGSME_CTZ(cell);
		northMask = CGSME_PROP_MASK[lut][DIR_IDX_N];
		eastMask = CGSME_PROP_MASK[lut][DIR_IDX_E];
		southMask = CGSME_PROP_MASK[lut][DIR_IDX_S];
		westMask = CGSME_PROP_MASK[lut][DIR_IDX_W];
	}
	else
	{
		// still a superposition -> no restriction
		northMask = All_Possible_State;
		eastMask = All_Possible_State;
		southMask = All_Possible_State;
		westMask = All_Possible_State;
	}

	// apply masks & check changes
	// ONLY access Heap if heap != NULL (allows to use this function in cleanup phases too)

	// WEST (x-1)
	if ((int32_t)x - 1 >= 0 && CGSME_POPCOUNT(gridLayer[y][x - 1]) > 1)
	{
		cgsme_tile_t oldVal = gridLayer[y][x - 1];
		gridLayer[y][x - 1] &= westMask;

		// REVIVAL CHECK: If it became 0, it was a contradiction. Reset it.
		if (gridLayer[y][x - 1] == 0)
			gridLayer[y][x - 1] = All_Possible_State;

		if (heap && gridLayer[y][x - 1] != oldVal)
			heapInsertOrUpdate(heap, gridLayer, x - 1, y, rng);
	}

	// EAST (x+1)
	if ((int32_t)x + 1 < width && CGSME_POPCOUNT(gridLayer[y][x + 1]) > 1)
	{
		cgsme_tile_t oldVal = gridLayer[y][x + 1];
		gridLayer[y][x + 1] &= eastMask;

		if (gridLayer[y][x + 1] == 0)
			gridLayer[y][x + 1] = All_Possible_State;

		if (heap && gridLayer[y][x + 1] != oldVal)
			heapInsertOrUpdate(heap, gridLayer, x + 1, y, rng);
	}

	// NORTH (y-1)
	if ((int32_t)y - 1 >= 0 && CGSME_POPCOUNT(gridLayer[y - 1][x]) > 1)
	{
		cgsme_tile_t oldVal = gridLayer[y - 1][x];
		gridLayer[y - 1][x] &= northMask;

		if (gridLayer[y - 1][x] == 0)
			gridLayer[y - 1][x] = All_Possible_State;

		if (heap && gridLayer[y - 1][x] != oldVal)
			heapInsertOrUpdate(heap, gridLayer, x, y - 1, rng);
	}

	// SOUTH (y+1)
	if ((int32_t)y + 1 < length && CGSME_POPCOUNT(gridLayer[y + 1][x]) > 1)
	{
		cgsme_tile_t oldVal = gridLayer[y + 1][x];
		gridLayer[y + 1][x] &= southMask;

		if (gridLayer[y + 1][x] == 0)
			gridLayer[y + 1][x] = All_Possible_State;

		if (heap && gridLayer[y + 1][x] != oldVal)
			heapInsertOrUpdate(heap, gridLayer, x, y + 1, rng);
	}

#ifdef cgsme_DEBUG
	if (cgsme_get_enabled())
	{
		uint64_t __cgsme_neigh_end = cgsme_now_us();
		uint64_t __cgsme_neigh_end_cycles = cgsme_now_cycles();
		cgsme_profile_record("updateNeighbours", (unsigned long long)(__cgsme_neigh_end - __cgsme_neigh_start),
							 (unsigned long long)(__cgsme_neigh_end_cycles - __cgsme_neigh_start_cycles));
	}
#endif
}

// scoring logic extracted to a helper
float calculateScore(cgsme_tile_t **grid, uint32_t x, uint32_t y, uint32_t *rng)
{
	CGSME_PROFILE_FUNC();
	uint32_t bitNum = CGSME_POPCOUNT(grid[y][x]);

	// entropy only        lower entropy = higher priority
	float score = (float)bitNum;

	// Tiny random noise to break ties
	float noise = ((float)nextRandom(rng) / 4294967296.0f) * 0.01f;
	return score + noise;
}

// constrain a reset cell using ONE neighbor
// dirFromNeighbor = direction from the neighbor toward us
//   e.g. our North neighbor sees us on ITS South side -> DIR_IDX_S
static inline void constrainFromNeighbor(cgsme_tile_t *cell, cgsme_tile_t neighbor, uint8_t dirFromNeighbor)
{
	if (neighbor == Empty_Tile)
		*cell &= CGSME_PROP_MASK[CGSME_NUM_TILES][dirFromNeighbor]; // void neighbor -> we must be closed toward it
	else if (CGSME_POPCOUNT(neighbor) == 1)
		*cell &= CGSME_PROP_MASK[CGSME_CTZ(neighbor)][dirFromNeighbor];
	// uncollapsed neighbor -> no constraint
}

// finds the best spot to start a new island
bool findBestSeedLocation(cgsme_tile_t **grid, uint32_t width, uint32_t length, uint32_t *outX, uint32_t *outY, uint32_t *rng)
{
	CGSME_PROFILE_FUNC();
	float minScore = 1e9;
	bool found = false;

	for (uint32_t i = 0; i < length; i++)
	{
		for (uint32_t j = 0; j < width; j++)
		{
			cgsme_tile_t tile = grid[i][j];

			// Skip Empty (void) tiles
			if (tile == Empty_Tile)
				continue;

			int popcount = CGSME_POPCOUNT(tile);

			// skip already collapsed
			if (popcount == 1)
				continue;

			// if popcount is 0 this tile hit a contradiction
			// reset AND immediately apply neighbor constraints
			if (popcount == 0)
			{
				grid[i][j] = All_Possible_State;

				if (i > 0)
					constrainFromNeighbor(&grid[i][j], grid[i - 1][j], DIR_IDX_S); // North neighbor
				if (i < length - 1)
					constrainFromNeighbor(&grid[i][j], grid[i + 1][j], DIR_IDX_N); // South neighbor
				if (j > 0)
					constrainFromNeighbor(&grid[i][j], grid[i][j - 1], DIR_IDX_E); // West neighbor
				if (j < width - 1)
					constrainFromNeighbor(&grid[i][j], grid[i][j + 1], DIR_IDX_W); // East neighbor

				// After applying constraints, check if still valid
				popcount = CGSME_POPCOUNT(grid[i][j]);
				if (popcount == 0)
				{
					// Still contradicted -> force All_Possible_State and return it as a seed.
					// germanWelder will fix any connection issue later.
					grid[i][j] = All_Possible_State;
					*outX = j;
					*outY = i;
					return true;
				}
				if (popcount == 1)
				{
					// Auto-collapsed to a single valid tile -> return it so the main loop
					// can call updateNeighbours and count it properly.
					*outX = j;
					*outY = i;
					return true;
				}
			}

			// Valid candidate (popcount > 1)
			// Use only random noise for seed selection - no distance preference
			float noise = (float)(((i * width + j) ^ nextRandom(rng)) & 0xFFFF) * 0.001f;

			if (noise < minScore)
			{
				minScore = noise;
				*outX = j;
				*outY = i;
				found = true;
			}
		}
	}

	return found;
}

void update_spawnrates(float rates[], int current_collapsed, int target_collapsed)
{
	CGSME_PROFILE_FUNC();

	// Progress = how much of the mask is filled (0.0 to 1.0)
	float progress = (target_collapsed > 0) ? ((float)current_collapsed / (float)target_collapsed) : 0.0f;
	if (progress > 1.0f)
		progress = 1.0f;

	// the peak slides from 0 to the furthest curve_pos of any naturally spawning category
	// (standard tileset: X=0 ... D=4)
	float max_pos = 0.0f;
	for (int i = 0; i < CGSME_NUM_CATEGORIES; ++i)
	{
		if (CGSME_CATEGORY_NATURAL[i] && CGSME_CATEGORY_CURVE_POS[i] > max_pos)
			max_pos = CGSME_CATEGORY_CURVE_POS[i];
	}

	float peak_position = max_pos * progress;
	float current_boost = CGSME_CONNECTOR_BOOST * (1.0f - progress); // strongest at start, fades to 0
	float raw_weights[CGSME_NUM_CATEGORIES];
	float total_weight = 0.0f;

	for (int i = 0; i < CGSME_NUM_CATEGORIES; ++i)
	{
		// categories like Special X are forbidden from natural spawning
		if (!CGSME_CATEGORY_NATURAL[i])
		{
			raw_weights[i] = 0.0f;
			continue;
		}

		// 1. standard gaussian weight
		float diff = CGSME_CATEGORY_CURVE_POS[i] - peak_position;
		float exponent = -(diff * diff) / (2.0f * CGSME_GAUSS_WIDTH * CGSME_GAUSS_WIDTH);
		raw_weights[i] = expf(exponent);

		// 2. connector boost (L and I in the standard tileset)
		if (CGSME_CATEGORY_BOOST[i])
			raw_weights[i] += current_boost;

		total_weight += raw_weights[i];
	}

	// 3. normalize
	if (total_weight > 0.0f)
	{
		for (int i = 0; i < CGSME_NUM_CATEGORIES; ++i)
			rates[i] = raw_weights[i] / total_weight;
	}
}
