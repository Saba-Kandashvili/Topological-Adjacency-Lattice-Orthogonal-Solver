#include "talos_solver.h"
#include "talos_debug.h"
#include "talos_utils.h"
#include <math.h>

// returns true if any collapsed neighbor has an open connection pointing to this tile
bool isTileRequired(talos_tile_t **grid, uint32_t width, uint32_t length, uint32_t x, uint32_t y)
{
	TALOS_PROFILE_FUNC();
	// check NORTH neighbor (y-1). Does it open SOUTH toward us?
	if (y > 0 && TALOS_POPCOUNT(grid[y - 1][x]) == 1)
	{
		if (grid[y - 1][x] & TALOS_HAS_PORT_MASK[DIR_IDX_S])
			return true;
	}

	// check SOUTH neighbor (y+1). Does it open NORTH toward us?
	if (y < length - 1 && TALOS_POPCOUNT(grid[y + 1][x]) == 1)
	{
		if (grid[y + 1][x] & TALOS_HAS_PORT_MASK[DIR_IDX_N])
			return true;
	}

	// check EAST neighbor (x+1). Does it open WEST toward us?
	if (x < width - 1 && TALOS_POPCOUNT(grid[y][x + 1]) == 1)
	{
		if (grid[y][x + 1] & TALOS_HAS_PORT_MASK[DIR_IDX_W])
			return true;
	}

	// check WEST neighbor (x-1). Does it open EAST toward us?
	if (x > 0 && TALOS_POPCOUNT(grid[y][x - 1]) == 1)
	{
		if (grid[y][x - 1] & TALOS_HAS_PORT_MASK[DIR_IDX_E])
			return true;
	}

	return false;
}

// picks ONE set bit out of `options`, weighted by the category rates
// iterates only over set bits: t &= (t - 1) clears the lowest set bit each step,
// TALOS_CTZ(t) gives the index of the current lowest set bit
static talos_tile_t pickWeightedTile(talos_tile_t options, const float *rates, uint32_t *rng)
{
	// sum weights of valid options
	float total_weight = 0.0f;
	for (talos_tile_t t = options; t != 0; t &= (t - 1))
		total_weight += rates[TALOS_BIT_TO_CATEGORY[TALOS_CTZ(t)]];

	// if all weights are ~0 just pick uniformly
	if (total_weight <= 0.0001f)
	{
		uint32_t r = nextRandom(rng) % (uint32_t)TALOS_POPCOUNT(options);
		talos_tile_t t = options;
		for (uint32_t k = 0; k < r; k++)
			t &= (t - 1);
		return TALOS_BIT(TALOS_CTZ(t));
	}

	float random_01 = (float)nextRandom(rng) / 4294967296.0f;
	float random_val = random_01 * total_weight;

	for (talos_tile_t t = options; t != 0; t &= (t - 1))
	{
		int i = TALOS_CTZ(t);
		random_val -= rates[TALOS_BIT_TO_CATEGORY[i]];
		if (random_val <= 0)
			return TALOS_BIT(i);
	}

	// float rounding fallback
	talos_tile_t last = options;
	while (last & (last - 1))
		last &= (last - 1);
	return last;
}

// collapseTile with spawnrates (smooth gaussian model that changes over time)
void collapseTile(talos_tile_t *tile, float *rates, uint32_t *rng)
{
#ifdef TALOS_DEBUG
	uint64_t __talos_collapse_start = talos_now_us();
	uint64_t __talos_collapse_start_cycles = talos_now_cycles();
#endif
	if (*tile == Empty_Tile)
		return;

	*tile = pickWeightedTile(*tile, rates, rng);

#ifdef TALOS_DEBUG
	if (talos_get_enabled())
	{
		uint64_t __talos_collapse_end = talos_now_us();
		uint64_t __talos_collapse_end_cycles = talos_now_cycles();
		talos_profile_record("collapseTile", (unsigned long long)(__talos_collapse_end - __talos_collapse_start),
							 (unsigned long long)(__talos_collapse_end_cycles - __talos_collapse_start_cycles));
	}
#endif
}

// contradiction fallback: keep void/edge closure and pick best match against collapsed neighbors
static talos_tile_t recoverCellCandidates(talos_tile_t **gridLayer, uint32_t width, uint32_t length, uint32_t x, uint32_t y)
{
	talos_tile_t voidOobMask = All_Possible_State;
	if (y == 0 || gridLayer[y - 1][x] == Empty_Tile)
		voidOobMask &= TALOS_PROP_MASK[TALOS_NUM_TILES][DIR_IDX_S];
	if (x + 1 >= width || gridLayer[y][x + 1] == Empty_Tile)
		voidOobMask &= TALOS_PROP_MASK[TALOS_NUM_TILES][DIR_IDX_W];
	if (y + 1 >= length || gridLayer[y + 1][x] == Empty_Tile)
		voidOobMask &= TALOS_PROP_MASK[TALOS_NUM_TILES][DIR_IDX_N];
	if (x == 0 || gridLayer[y][x - 1] == Empty_Tile)
		voidOobMask &= TALOS_PROP_MASK[TALOS_NUM_TILES][DIR_IDX_E];

	if (voidOobMask == Empty_Tile)
		return Empty_Tile;

	talos_tile_t nbMasks[4];
	int nbCount = 0;

	if (y > 0 && gridLayer[y - 1][x] != Empty_Tile && TALOS_POPCOUNT(gridLayer[y - 1][x]) == 1)
		nbMasks[nbCount++] = TALOS_PROP_MASK[TALOS_CTZ(gridLayer[y - 1][x])][DIR_IDX_S];
	if (x + 1 < width && gridLayer[y][x + 1] != Empty_Tile && TALOS_POPCOUNT(gridLayer[y][x + 1]) == 1)
		nbMasks[nbCount++] = TALOS_PROP_MASK[TALOS_CTZ(gridLayer[y][x + 1])][DIR_IDX_W];
	if (y + 1 < length && gridLayer[y + 1][x] != Empty_Tile && TALOS_POPCOUNT(gridLayer[y + 1][x]) == 1)
		nbMasks[nbCount++] = TALOS_PROP_MASK[TALOS_CTZ(gridLayer[y + 1][x])][DIR_IDX_N];
	if (x > 0 && gridLayer[y][x - 1] != Empty_Tile && TALOS_POPCOUNT(gridLayer[y][x - 1]) == 1)
		nbMasks[nbCount++] = TALOS_PROP_MASK[TALOS_CTZ(gridLayer[y][x - 1])][DIR_IDX_E];

	if (nbCount == 0)
		return voidOobMask;

	int bestScore = -1;
	talos_tile_t bestMask = Empty_Tile;

	for (talos_tile_t rem = voidOobMask; rem != 0; rem &= (rem - 1))
	{
		int bitIdx = TALOS_CTZ(rem);
		talos_tile_t bit = TALOS_BIT(bitIdx);
		int matches = 0;
		for (int k = 0; k < nbCount; k++)
		{
			if (nbMasks[k] & bit)
				matches++;
		}
		int score = matches * 2 + (TALOS_CATEGORY_NATURAL[TALOS_BIT_TO_CATEGORY[bitIdx]] ? 1 : 0);
		if (score > bestScore)
		{
			bestScore = score;
			bestMask = bit;
		}
		else if (score == bestScore)
		{
			bestMask |= bit;
		}
	}

	return (bestMask != Empty_Tile) ? bestMask : voidOobMask;
}

talos_tile_t exactCellValidMask(talos_tile_t **gridLayer, uint32_t width, uint32_t length, uint32_t x, uint32_t y)
{
	talos_tile_t mask = All_Possible_State;
	if (y == 0 || gridLayer[y - 1][x] == Empty_Tile)
		mask &= TALOS_PROP_MASK[TALOS_NUM_TILES][DIR_IDX_S];
	else if (TALOS_POPCOUNT(gridLayer[y - 1][x]) == 1)
		mask &= TALOS_PROP_MASK[TALOS_CTZ(gridLayer[y - 1][x])][DIR_IDX_S];

	if (x + 1 >= width || gridLayer[y][x + 1] == Empty_Tile)
		mask &= TALOS_PROP_MASK[TALOS_NUM_TILES][DIR_IDX_W];
	else if (TALOS_POPCOUNT(gridLayer[y][x + 1]) == 1)
		mask &= TALOS_PROP_MASK[TALOS_CTZ(gridLayer[y][x + 1])][DIR_IDX_W];

	if (y + 1 >= length || gridLayer[y + 1][x] == Empty_Tile)
		mask &= TALOS_PROP_MASK[TALOS_NUM_TILES][DIR_IDX_N];
	else if (TALOS_POPCOUNT(gridLayer[y + 1][x]) == 1)
		mask &= TALOS_PROP_MASK[TALOS_CTZ(gridLayer[y + 1][x])][DIR_IDX_N];

	if (x == 0 || gridLayer[y][x - 1] == Empty_Tile)
		mask &= TALOS_PROP_MASK[TALOS_NUM_TILES][DIR_IDX_E];
	else if (TALOS_POPCOUNT(gridLayer[y][x - 1]) == 1)
		mask &= TALOS_PROP_MASK[TALOS_CTZ(gridLayer[y][x - 1])][DIR_IDX_E];

	return mask;
}

static bool backtrackPatch(
	talos_tile_t **gridLayer, uint32_t width, uint32_t length,
	uint32_t cx, uint32_t cy, uint32_t radius, MinHeap *heap, uint32_t *rng)
{
	static const int32_t dx[4] = {  0,  1,  0, -1 };
	static const int32_t dy[4] = { -1,  0,  1,  0 };

	uint32_t x0 = (cx > radius) ? (cx - radius) : 0;
	uint32_t y0 = (cy > radius) ? (cy - radius) : 0;
	uint32_t x1 = (cx + radius < width) ? (cx + radius) : (width - 1);
	uint32_t y1 = (cy + radius < length) ? (cy + radius) : (length - 1);

	for (uint32_t py = y0; py <= y1; py++)
	{
		for (uint32_t px = x0; px <= x1; px++)
		{
			if (gridLayer[py][px] != Empty_Tile)
				gridLayer[py][px] = All_Possible_State;
		}
	}

	for (uint32_t py = y0; py <= y1; py++)
	{
		for (uint32_t px = x0; px <= x1; px++)
		{
			if (gridLayer[py][px] == Empty_Tile)
				continue;
			talos_tile_t exact = exactCellValidMask(gridLayer, width, length, px, py);
			if (exact == Empty_Tile)
				return false;
			gridLayer[py][px] = exact;
		}
	}

	bool changed = true;
	int passes = 0;
	int maxPasses = (int)((x1 - x0 + 1) * (y1 - y0 + 1) * 2);
	while (changed && passes < maxPasses)
	{
		changed = false;
		passes++;
		for (uint32_t py = y0; py <= y1; py++)
		{
			for (uint32_t px = x0; px <= x1; px++)
			{
				talos_tile_t cell = gridLayer[py][px];
				if (cell == Empty_Tile)
					continue;

				talos_tile_t u[4] = { Empty_Tile, Empty_Tile, Empty_Tile, Empty_Tile };
				for (talos_tile_t tmp = cell; tmp != 0; tmp &= (tmp - 1))
				{
					uint32_t tIdx = (uint32_t)TALOS_CTZ(tmp);
					u[0] |= TALOS_PROP_MASK[tIdx][0];
					u[1] |= TALOS_PROP_MASK[tIdx][1];
					u[2] |= TALOS_PROP_MASK[tIdx][2];
					u[3] |= TALOS_PROP_MASK[tIdx][3];
				}

				for (int d = 0; d < 4; d++)
				{
					int32_t nx = (int32_t)px + dx[d];
					int32_t ny = (int32_t)py + dy[d];
					if (nx < (int32_t)x0 || nx > (int32_t)x1 || ny < (int32_t)y0 || ny > (int32_t)y1)
						continue;
					talos_tile_t oldN = gridLayer[ny][nx];
					if (oldN == Empty_Tile)
						continue;
					talos_tile_t newN = oldN & u[d];
					if (newN == Empty_Tile)
						return false;
					if (newN != oldN)
					{
						gridLayer[ny][nx] = newN;
						changed = true;
					}
				}
			}
		}
	}

	if (heap)
	{
		for (uint32_t py = y0; py <= y1; py++)
		{
			for (uint32_t px = x0; px <= x1; px++)
			{
				if (TALOS_POPCOUNT(gridLayer[py][px]) > 1)
					heapInsertOrUpdate(heap, gridLayer, px, py, rng);
			}
		}
	}

	return true;
}

void backtrackNeighborhood(
	talos_tile_t **gridLayer, uint32_t width, uint32_t length,
	uint32_t cx, uint32_t cy, MinHeap *heap, uint32_t *rng)
{
	for (uint32_t r = 1; r <= 4; r++)
	{
		if (backtrackPatch(gridLayer, width, length, cx, cy, r, heap, rng))
			return;
	}

	uint32_t x0 = (cx > 4) ? (cx - 4) : 0;
	uint32_t y0 = (cy > 4) ? (cy - 4) : 0;
	uint32_t x1 = (cx + 4 < width) ? (cx + 4) : (width - 1);
	uint32_t y1 = (cy + 4 < length) ? (cy + 4) : (length - 1);
	for (uint32_t py = y0; py <= y1; py++)
	{
		for (uint32_t px = x0; px <= x1; px++)
		{
			if (gridLayer[py][px] != Empty_Tile)
			{
				gridLayer[py][px] = recoverCellCandidates(gridLayer, width, length, px, py);
				if (heap && TALOS_POPCOUNT(gridLayer[py][px]) > 1)
					heapInsertOrUpdate(heap, gridLayer, px, py, rng);
			}
		}
	}
}

typedef struct
{
	uint32_t x, y;
} SolverUnitPoint;

#define TALOS_UNIT_PROP_CAP 1024

static inline void propagateCellImmediate(
	talos_tile_t **gridLayer, uint32_t width, uint32_t length,
	uint32_t cx, uint32_t cy, MinHeap *heap, uint32_t *rng,
	SolverUnitPoint *queue, int *tail)
{
	static const int32_t dx[4] = {  0,  1,  0, -1 }; // N, E, S, W
	static const int32_t dy[4] = { -1,  0,  1,  0 };

	talos_tile_t cell = gridLayer[cy][cx];
	talos_tile_t unionMasks[4];
	const talos_tile_t *dirMasks = NULL;

	if (cell == Empty_Tile || TALOS_POPCOUNT(cell) == 1)
	{
		// row TALOS_NUM_TILES of TALOS_PROP_MASK is the Empty_Tile (void) row
		uint32_t lut = (cell == Empty_Tile) ? (uint32_t)TALOS_NUM_TILES : (uint32_t)TALOS_CTZ(cell);
		dirMasks = TALOS_PROP_MASK[lut];
	}
	else if (TALOS_NUM_SOCKETS > 2)
	{
		// union valid neighbor masks over remaining candidates (for multi-socket tilesets)
		unionMasks[0] = Empty_Tile;
		unionMasks[1] = Empty_Tile;
		unionMasks[2] = Empty_Tile;
		unionMasks[3] = Empty_Tile;
		for (talos_tile_t tmp = cell; tmp != 0; tmp &= (tmp - 1))
		{
			uint32_t tIdx = (uint32_t)TALOS_CTZ(tmp);
			unionMasks[0] |= TALOS_PROP_MASK[tIdx][0];
			unionMasks[1] |= TALOS_PROP_MASK[tIdx][1];
			unionMasks[2] |= TALOS_PROP_MASK[tIdx][2];
			unionMasks[3] |= TALOS_PROP_MASK[tIdx][3];
		}
		dirMasks = unionMasks;
	}
	else
	{
		return;
	}

	for (int d = 0; d < 4; d++)
	{
		if (dirMasks[d] == All_Possible_State)
			continue;

		int32_t nx = (int32_t)cx + dx[d];
		int32_t ny = (int32_t)cy + dy[d];
		if (nx < 0 || ny < 0 || (uint32_t)nx >= width || (uint32_t)ny >= length)
			continue;

		talos_tile_t oldVal = gridLayer[ny][nx];
		if (oldVal == Empty_Tile)
			continue;

		if (TALOS_POPCOUNT(oldVal) == 1)
		{
			if (TALOS_NUM_SOCKETS > 2 && (oldVal & dirMasks[d]) == 0)
			{
				backtrackNeighborhood(gridLayer, width, length, (uint32_t)nx, (uint32_t)ny, heap, rng);
				*tail = 0;
				return;
			}
			continue;
		}

		talos_tile_t newVal = oldVal & dirMasks[d];
		if (newVal == 0)
		{
			if (TALOS_NUM_SOCKETS > 2)
			{
				talos_tile_t exact = exactCellValidMask(gridLayer, width, length, (uint32_t)nx, (uint32_t)ny) & dirMasks[d];
				if (exact != Empty_Tile)
				{
					newVal = exact;
				}
				else
				{
					backtrackNeighborhood(gridLayer, width, length, (uint32_t)nx, (uint32_t)ny, heap, rng);
					*tail = 0;
					return;
				}
			}
			else
			{
				newVal = recoverCellCandidates(gridLayer, width, length, (uint32_t)nx, (uint32_t)ny);
			}
		}
		gridLayer[ny][nx] = newVal;

		if (newVal != oldVal)
		{
			if (TALOS_POPCOUNT(newVal) <= 1 || TALOS_NUM_SOCKETS > 2)
			{
				if (*tail < TALOS_UNIT_PROP_CAP)
					queue[(*tail)++] = (SolverUnitPoint){ (uint32_t)nx, (uint32_t)ny };
			}
			if (TALOS_POPCOUNT(newVal) > 1 && heap)
			{
				heapInsertOrUpdate(heap, gridLayer, (uint32_t)nx, (uint32_t)ny, rng);
			}
		}
	}
}

void updateNeighbours(talos_tile_t **gridLayer, uint32_t width, uint32_t length, uint32_t x, uint32_t y, MinHeap *heap, uint32_t *rng)
{
#ifdef TALOS_DEBUG
	uint64_t __talos_neigh_start = talos_now_us();
	uint64_t __talos_neigh_start_cycles = talos_now_cycles();
#endif

	SolverUnitPoint queue[TALOS_UNIT_PROP_CAP];
	int head = 0, tail = 0;
	propagateCellImmediate(gridLayer, width, length, x, y, heap, rng, queue, &tail);

	while (head < tail)
	{
		SolverUnitPoint cur = queue[head++];
		propagateCellImmediate(gridLayer, width, length, cur.x, cur.y, heap, rng, queue, &tail);
	}

#ifdef TALOS_DEBUG
	if (talos_get_enabled())
	{
		uint64_t __talos_neigh_end = talos_now_us();
		uint64_t __talos_neigh_end_cycles = talos_now_cycles();
		talos_profile_record("updateNeighbours", (unsigned long long)(__talos_neigh_end - __talos_neigh_start),
							 (unsigned long long)(__talos_neigh_end_cycles - __talos_neigh_start_cycles));
	}
#endif
}

// scoring logic extracted to a helper
float calculateScore(talos_tile_t **grid, uint32_t x, uint32_t y, uint32_t *rng)
{
	TALOS_PROFILE_FUNC();
	uint32_t bitNum = TALOS_POPCOUNT(grid[y][x]);

	// entropy only        lower entropy = higher priority
	float score = (float)bitNum;

	// Tiny random noise to break ties
	float noise = ((float)nextRandom(rng) / 4294967296.0f) * 0.01f;
	return score + noise;
}

// finds the best spot to start a new island
bool findBestSeedLocation(talos_tile_t **grid, uint32_t width, uint32_t length, uint32_t *outX, uint32_t *outY, uint32_t *rng)
{
	TALOS_PROFILE_FUNC();
	float minScore = 1e9;
	bool found = false;

	for (uint32_t i = 0; i < length; i++)
	{
		for (uint32_t j = 0; j < width; j++)
		{
			talos_tile_t tile = grid[i][j];

			// Skip Empty (void) tiles
			if (tile == Empty_Tile)
				continue;

			int popcount = TALOS_POPCOUNT(tile);

			// skip already collapsed
			if (popcount == 1)
				continue;

			// if popcount is 0 this tile hit a contradiction
			// recover cleanly against Void, OOB, and already-collapsed neighbors
			if (popcount == 0)
			{
				grid[i][j] = recoverCellCandidates(grid, width, length, j, i);
				popcount = TALOS_POPCOUNT(grid[i][j]);
				if (grid[i][j] == Empty_Tile)
					continue;
				if (popcount == 1)
				{
					// collapsed to 1 tile, hand it back so main loop updates neighbors
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
	TALOS_PROFILE_FUNC();

	// Progress = how much of the mask is filled (0.0 to 1.0)
	float progress = (target_collapsed > 0) ? ((float)current_collapsed / (float)target_collapsed) : 0.0f;
	if (progress > 1.0f)
		progress = 1.0f;

	// the peak slides from 0 to the furthest curve_pos of any naturally spawning category
	// (standard tileset: X=0 ... D=4)
	float max_pos = 0.0f;
	for (int i = 0; i < TALOS_NUM_CATEGORIES; ++i)
	{
		if (TALOS_CATEGORY_NATURAL[i] && TALOS_CATEGORY_CURVE_POS[i] > max_pos)
			max_pos = TALOS_CATEGORY_CURVE_POS[i];
	}

	float peak_position = max_pos * progress;
	float current_boost = TALOS_CONNECTOR_BOOST * (1.0f - progress); // strongest at start, fades to 0
	float raw_weights[TALOS_MAX_CATEGORIES];
	float total_weight = 0.0f;

	for (int i = 0; i < TALOS_NUM_CATEGORIES; ++i)
	{
		// categories like Special X are forbidden from natural spawning
		if (!TALOS_CATEGORY_NATURAL[i])
		{
			raw_weights[i] = 0.0f;
			continue;
		}

		// gaussian weight
		float diff = TALOS_CATEGORY_CURVE_POS[i] - peak_position;
		float exponent = -(diff * diff) / (2.0f * TALOS_GAUSS_WIDTH * TALOS_GAUSS_WIDTH);
		raw_weights[i] = expf(exponent);

		// connector boost (L and I in standard tileset)
		if (TALOS_CATEGORY_BOOST[i])
			raw_weights[i] += current_boost;

		total_weight += raw_weights[i];
	}

	// normalize
	if (total_weight > 0.0f)
	{
		for (int i = 0; i < TALOS_NUM_CATEGORIES; ++i)
			rates[i] = raw_weights[i] / total_weight;
	}
}

// 3d chunk heap and 6-way propagation (N E S W U D)

ChunkHeap3D *initChunkHeap3D(uint32_t max_cw, uint32_t max_cl, uint32_t height)
{
	ChunkHeap3D *h = malloc(sizeof(ChunkHeap3D));
	if (!h)
		return NULL;
	uint32_t cap = max_cw * max_cl * height;
	if (cap == 0)
		cap = 1;
	h->capacity = cap;
	h->count = 0;
	h->x0 = 0;
	h->y0 = 0;
	h->cw = max_cw;
	h->cl = max_cl;
	h->height = height;
	h->nodes = malloc(sizeof(ChunkHeapNode3D) * cap);
	h->indexMap = malloc(sizeof(int32_t) * cap);
	if (!h->nodes || !h->indexMap)
	{
		free(h->nodes);
		free(h->indexMap);
		free(h);
		return NULL;
	}
	for (uint32_t i = 0; i < cap; i++)
		h->indexMap[i] = -1;
	return h;
}

void freeChunkHeap3D(ChunkHeap3D *h)
{
	if (!h)
		return;
	free(h->nodes);
	free(h->indexMap);
	free(h);
}

void bindChunkHeap3D(ChunkHeap3D *h, uint32_t x0, uint32_t y0, uint32_t cw, uint32_t cl, uint32_t height)
{
	h->x0 = x0;
	h->y0 = y0;
	h->cw = cw;
	h->cl = cl;
	h->height = height;
	h->count = 0;
	uint32_t total = cw * cl * height;
	if (total > h->capacity)
		total = h->capacity;
	for (uint32_t i = 0; i < total; i++)
		h->indexMap[i] = -1;
}

static inline void swapChunkNodes3D(ChunkHeap3D *h, uint32_t i, uint32_t j)
{
	ChunkHeapNode3D tmp = h->nodes[i];
	h->nodes[i] = h->nodes[j];
	h->nodes[j] = tmp;

	uint32_t idx_i = ((uint32_t)h->nodes[i].z * h->cl + (uint32_t)h->nodes[i].ly) * h->cw + (uint32_t)h->nodes[i].lx;
	uint32_t idx_j = ((uint32_t)h->nodes[j].z * h->cl + (uint32_t)h->nodes[j].ly) * h->cw + (uint32_t)h->nodes[j].lx;
	h->indexMap[idx_i] = (int32_t)i;
	h->indexMap[idx_j] = (int32_t)j;
}

static inline void bubbleUpChunk3D(ChunkHeap3D *h, uint32_t index)
{
	while (index > 0)
	{
		uint32_t parent = (index - 1) / 2;
		if (h->nodes[index].score < h->nodes[parent].score)
		{
			swapChunkNodes3D(h, index, parent);
			index = parent;
		}
		else
		{
			break;
		}
	}
}

static inline void bubbleDownChunk3D(ChunkHeap3D *h, uint32_t index)
{
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
			swapChunkNodes3D(h, index, smallest);
			index = smallest;
		}
		else
		{
			break;
		}
	}
}

void chunkHeapInsertOrUpdate3D(ChunkHeap3D *h, talos_tile_t ***grid, uint32_t x, uint32_t y, uint32_t z, uint32_t *rng)
{
	if (!h || x < h->x0 || x >= h->x0 + h->cw || y < h->y0 || y >= h->y0 + h->cl || z >= h->height)
		return;
	if (TALOS_POPCOUNT(grid[z][y][x]) <= 1)
		return;

	uint32_t lx = x - h->x0;
	uint32_t ly = y - h->y0;
	uint32_t mapIdx = (z * h->cl + ly) * h->cw + lx;
	if (mapIdx >= h->capacity)
		return;

	float score = calculateScore(grid[z], x, y, rng);
	int32_t curIdx = h->indexMap[mapIdx];
	if (curIdx != -1)
	{
		if (score < h->nodes[curIdx].score)
		{
			h->nodes[curIdx].score = score;
			bubbleUpChunk3D(h, (uint32_t)curIdx);
		}
		else if (score > h->nodes[curIdx].score)
		{
			h->nodes[curIdx].score = score;
			bubbleDownChunk3D(h, (uint32_t)curIdx);
		}
	}
	else if (h->count < h->capacity)
	{
		uint32_t idx = h->count++;
		h->nodes[idx].lx = (uint16_t)lx;
		h->nodes[idx].ly = (uint16_t)ly;
		h->nodes[idx].z  = (uint16_t)z;
		h->nodes[idx].score = score;
		h->indexMap[mapIdx] = (int32_t)idx;
		bubbleUpChunk3D(h, idx);
	}
}

bool chunkHeapPop3D(ChunkHeap3D *h, talos_tile_t ***grid, uint32_t *outX, uint32_t *outY, uint32_t *outZ)
{
	while (h && h->count > 0)
	{
		ChunkHeapNode3D top = h->nodes[0];
		uint32_t lastIdx = h->count - 1;
		swapChunkNodes3D(h, 0, lastIdx);
		h->count--;
		uint32_t mapIdx = ((uint32_t)top.z * h->cl + (uint32_t)top.ly) * h->cw + (uint32_t)top.lx;
		h->indexMap[mapIdx] = -1;

		if (h->count > 0)
			bubbleDownChunk3D(h, 0);

		uint32_t x = h->x0 + (uint32_t)top.lx;
		uint32_t y = h->y0 + (uint32_t)top.ly;
		uint32_t z = (uint32_t)top.z;
		if (TALOS_POPCOUNT(grid[z][y][x]) > 1)
		{
			*outX = x;
			*outY = y;
			*outZ = z;
			return true;
		}
	}
	return false;
}

talos_tile_t exactCellValidMask3D(talos_tile_t ***grid, uint32_t width, uint32_t length, uint32_t height, uint32_t x, uint32_t y, uint32_t z)
{
	talos_tile_t mask = exactCellValidMask(grid[z], width, length, x, y);
	if (height > 1)
	{
		if (z + 1 >= height || grid[z + 1][y][x] == Empty_Tile)
			mask &= TALOS_VERT_PROP_MASK[TALOS_NUM_TILES][VERT_IDX_DOWN];
		else if (TALOS_POPCOUNT(grid[z + 1][y][x]) == 1)
			mask &= TALOS_VERT_PROP_MASK[TALOS_CTZ(grid[z + 1][y][x])][VERT_IDX_DOWN];

		if (z == 0 || grid[z - 1][y][x] == Empty_Tile)
			mask &= TALOS_VERT_PROP_MASK[TALOS_NUM_TILES][VERT_IDX_UP];
		else if (TALOS_POPCOUNT(grid[z - 1][y][x]) == 1)
			mask &= TALOS_VERT_PROP_MASK[TALOS_CTZ(grid[z - 1][y][x])][VERT_IDX_UP];
	}
	return mask;
}

static talos_tile_t recoverCellCandidates3D(talos_tile_t ***grid, uint32_t width, uint32_t length, uint32_t height, uint32_t x, uint32_t y, uint32_t z)
{
	talos_tile_t exact = exactCellValidMask3D(grid, width, length, height, x, y, z);
	if (exact != Empty_Tile)
		return exact;

	talos_tile_t base = recoverCellCandidates(grid[z], width, length, x, y);
	if (height > 1)
	{
		talos_tile_t vMask = All_Possible_State;
		if (z + 1 >= height || grid[z + 1][y][x] == Empty_Tile)
			vMask &= TALOS_VERT_PROP_MASK[TALOS_NUM_TILES][VERT_IDX_DOWN];
		else if (TALOS_POPCOUNT(grid[z + 1][y][x]) == 1)
			vMask &= TALOS_VERT_PROP_MASK[TALOS_CTZ(grid[z + 1][y][x])][VERT_IDX_DOWN];

		if (z == 0 || grid[z - 1][y][x] == Empty_Tile)
			vMask &= TALOS_VERT_PROP_MASK[TALOS_NUM_TILES][VERT_IDX_UP];
		else if (TALOS_POPCOUNT(grid[z - 1][y][x]) == 1)
			vMask &= TALOS_VERT_PROP_MASK[TALOS_CTZ(grid[z - 1][y][x])][VERT_IDX_UP];

		if ((base & vMask) != Empty_Tile)
			base &= vMask;
		else if (vMask != Empty_Tile)
			base = vMask;
	}
	return base;
}

static bool backtrackChunkPatch3D(
	talos_tile_t ***grid, uint32_t width, uint32_t length, uint32_t height,
	ChunkHeap3D *heap, uint32_t cx, uint32_t cy, uint32_t cz, uint32_t radius, uint32_t *rng)
{
	static const int32_t dx[4] = {  0,  1,  0, -1 };
	static const int32_t dy[4] = { -1,  0,  1,  0 };

	uint32_t bx0 = heap ? heap->x0 : 0;
	uint32_t by0 = heap ? heap->y0 : 0;
	uint32_t bx1 = heap ? (heap->x0 + heap->cw - 1) : (width - 1);
	uint32_t by1 = heap ? (heap->y0 + heap->cl - 1) : (length - 1);

	uint32_t x0 = (cx > bx0 + radius) ? (cx - radius) : bx0;
	uint32_t y0 = (cy > by0 + radius) ? (cy - radius) : by0;
	uint32_t x1 = (cx + radius < bx1) ? (cx + radius) : bx1;
	uint32_t y1 = (cy + radius < by1) ? (cy + radius) : by1;

	talos_tile_t naturalMask = Empty_Tile;
	bool hasNatVert = false;
	for (int t = 0; t < TALOS_NUM_TILES; t++)
	{
		if (TALOS_CATEGORY_NATURAL[TALOS_BIT_TO_CATEGORY[t]])
		{
			naturalMask |= TALOS_BIT(t);
			if ((TALOS_VERT_PROP_MASK[t][0] & naturalMask) != naturalMask ||
				(TALOS_VERT_PROP_MASK[t][1] & naturalMask) != naturalMask)
				hasNatVert = true;
		}
	}
	if (naturalMask == Empty_Tile)
		naturalMask = All_Possible_State;

	uint32_t zRad = (hasNatVert && height > 1) ? 1u : 0u;
	uint32_t z0 = (cz > zRad) ? (cz - zRad) : 0u;
	uint32_t z1 = (cz + zRad < height) ? (cz + zRad) : (height - 1u);

	for (uint32_t pz = z0; pz <= z1; pz++)
	{
		for (uint32_t py = y0; py <= y1; py++)
		{
			for (uint32_t px = x0; px <= x1; px++)
			{
				talos_tile_t cur = grid[pz][py][px];
				if (cur == Empty_Tile)
					continue;
				if (TALOS_POPCOUNT(cur) == 1 && (cur & naturalMask) == Empty_Tile)
					continue;
				grid[pz][py][px] = naturalMask;
			}
		}
	}

	for (uint32_t pz = z0; pz <= z1; pz++)
	{
		for (uint32_t py = y0; py <= y1; py++)
		{
			for (uint32_t px = x0; px <= x1; px++)
			{
				talos_tile_t cur = grid[pz][py][px];
				if (cur == Empty_Tile)
					continue;
				if (TALOS_POPCOUNT(cur) == 1 && (cur & naturalMask) == Empty_Tile)
					continue;
				talos_tile_t exact = exactCellValidMask3D(grid, width, length, height, px, py, pz) & naturalMask;
				if (exact == Empty_Tile)
					return false;
				grid[pz][py][px] = exact;
			}
		}
	}

	bool changed = true;
	int passes = 0;
	int maxPasses = (int)((x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) * 2);
	while (changed && passes < maxPasses)
	{
		changed = false;
		passes++;
		for (uint32_t pz = z0; pz <= z1; pz++)
		{
			for (uint32_t py = y0; py <= y1; py++)
			{
				for (uint32_t px = x0; px <= x1; px++)
				{
					talos_tile_t cell = grid[pz][py][px];
					if (cell == Empty_Tile)
						continue;

					talos_tile_t u[4] = { Empty_Tile, Empty_Tile, Empty_Tile, Empty_Tile };
					talos_tile_t vu[2] = { Empty_Tile, Empty_Tile };
					for (talos_tile_t tmp = cell; tmp != 0; tmp &= (tmp - 1))
					{
						uint32_t tIdx = (uint32_t)TALOS_CTZ(tmp);
						u[0] |= TALOS_PROP_MASK[tIdx][0];
						u[1] |= TALOS_PROP_MASK[tIdx][1];
						u[2] |= TALOS_PROP_MASK[tIdx][2];
						u[3] |= TALOS_PROP_MASK[tIdx][3];
						vu[0] |= TALOS_VERT_PROP_MASK[tIdx][0];
						vu[1] |= TALOS_VERT_PROP_MASK[tIdx][1];
					}

					for (int d = 0; d < 4; d++)
					{
						int32_t nx = (int32_t)px + dx[d];
						int32_t ny = (int32_t)py + dy[d];
						if (nx < (int32_t)x0 || nx > (int32_t)x1 || ny < (int32_t)y0 || ny > (int32_t)y1)
							continue;
						talos_tile_t oldN = grid[pz][ny][nx];
						if (oldN == Empty_Tile)
							continue;
						talos_tile_t newN = oldN & u[d];
						if (newN == Empty_Tile)
							return false;
						if (newN != oldN)
						{
							grid[pz][ny][nx] = newN;
							changed = true;
						}
					}

					if (z1 > z0)
					{
						for (int vd = 0; vd < 2; vd++)
						{
							int32_t nz = (vd == VERT_IDX_UP) ? ((int32_t)pz + 1) : ((int32_t)pz - 1);
							if (nz < (int32_t)z0 || nz > (int32_t)z1)
								continue;
							talos_tile_t oldN = grid[nz][py][px];
							if (oldN == Empty_Tile)
								continue;
							talos_tile_t newN = oldN & vu[vd];
							if (newN == Empty_Tile)
								return false;
							if (newN != oldN)
							{
								grid[nz][py][px] = newN;
								changed = true;
							}
						}
					}
				}
			}
		}
	}

	if (heap)
	{
		for (uint32_t pz = z0; pz <= z1; pz++)
		{
			for (uint32_t py = y0; py <= y1; py++)
			{
				for (uint32_t px = x0; px <= x1; px++)
				{
					if (TALOS_POPCOUNT(grid[pz][py][px]) > 1)
						chunkHeapInsertOrUpdate3D(heap, grid, px, py, pz, rng);
				}
			}
		}
	}
	return true;
}

void backtrackChunkNeighborhood3D(
	talos_tile_t ***grid, uint32_t width, uint32_t length, uint32_t height,
	ChunkHeap3D *heap, uint32_t cx, uint32_t cy, uint32_t cz, uint32_t *rng)
{
	for (uint32_t r = 1; r <= 4; r++)
	{
		if (backtrackChunkPatch3D(grid, width, length, height, heap, cx, cy, cz, r, rng))
			return;
	}

	uint32_t bx0 = heap ? heap->x0 : 0;
	uint32_t by0 = heap ? heap->y0 : 0;
	uint32_t bx1 = heap ? (heap->x0 + heap->cw - 1) : (width - 1);
	uint32_t by1 = heap ? (heap->y0 + heap->cl - 1) : (length - 1);

	uint32_t x0 = (cx > bx0 + 3) ? (cx - 3) : bx0;
	uint32_t y0 = (cy > by0 + 3) ? (cy - 3) : by0;
	uint32_t x1 = (cx + 3 < bx1) ? (cx + 3) : bx1;
	uint32_t y1 = (cy + 3 < by1) ? (cy + 3) : by1;

	for (uint32_t py = y0; py <= y1; py++)
	{
		for (uint32_t px = x0; px <= x1; px++)
		{
			if (grid[cz][py][px] != Empty_Tile)
			{
				grid[cz][py][px] = recoverCellCandidates3D(grid, width, length, height, px, py, cz);
				if (heap && TALOS_POPCOUNT(grid[cz][py][px]) > 1)
					chunkHeapInsertOrUpdate3D(heap, grid, px, py, cz, rng);
			}
		}
	}
}

typedef struct
{
	uint16_t x, y, z;
} SolverUnitPoint3D;

static inline void propagateCellImmediate3D(
	talos_tile_t ***grid, uint32_t width, uint32_t length, uint32_t height,
	ChunkHeap3D *heap, uint32_t cx, uint32_t cy, uint32_t cz, uint32_t *rng,
	SolverUnitPoint3D *queue, int *tail)
{
	static const int32_t dx[4] = {  0,  1,  0, -1 }; // N, E, S, W
	static const int32_t dy[4] = { -1,  0,  1,  0 };

	uint32_t bx0 = heap ? heap->x0 : 0;
	uint32_t by0 = heap ? heap->y0 : 0;
	uint32_t bx1 = heap ? (heap->x0 + heap->cw - 1) : (width - 1);
	uint32_t by1 = heap ? (heap->y0 + heap->cl - 1) : (length - 1);

	talos_tile_t cell = grid[cz][cy][cx];
	talos_tile_t unionMasks[4];
	talos_tile_t vertUnion[2];
	const talos_tile_t *dirMasks = NULL;
	const talos_tile_t *vertMasks = NULL;
	bool isMultiRule = (TALOS_NUM_SOCKETS > 2) ||
		(TALOS_VERT_PROP_MASK[0][0] != All_Possible_State && TALOS_VERT_PROP_MASK[0][1] != All_Possible_State);

	if (cell == Empty_Tile || TALOS_POPCOUNT(cell) == 1)
	{
		uint32_t lut = (cell == Empty_Tile) ? (uint32_t)TALOS_NUM_TILES : (uint32_t)TALOS_CTZ(cell);
		dirMasks  = TALOS_PROP_MASK[lut];
		vertMasks = TALOS_VERT_PROP_MASK[lut];
	}
	else if (isMultiRule)
	{
		unionMasks[0] = Empty_Tile;
		unionMasks[1] = Empty_Tile;
		unionMasks[2] = Empty_Tile;
		unionMasks[3] = Empty_Tile;
		vertUnion[0]  = Empty_Tile;
		vertUnion[1]  = Empty_Tile;
		for (talos_tile_t tmp = cell; tmp != 0; tmp &= (tmp - 1))
		{
			uint32_t tIdx = (uint32_t)TALOS_CTZ(tmp);
			unionMasks[0] |= TALOS_PROP_MASK[tIdx][0];
			unionMasks[1] |= TALOS_PROP_MASK[tIdx][1];
			unionMasks[2] |= TALOS_PROP_MASK[tIdx][2];
			unionMasks[3] |= TALOS_PROP_MASK[tIdx][3];
			vertUnion[0]  |= TALOS_VERT_PROP_MASK[tIdx][0];
			vertUnion[1]  |= TALOS_VERT_PROP_MASK[tIdx][1];
		}
		dirMasks  = unionMasks;
		vertMasks = vertUnion;
	}
	else
	{
		return;
	}

	// horizontal propagation (N E S W) inside chunk bounds
	for (int d = 0; d < 4; d++)
	{
		if (dirMasks[d] == All_Possible_State)
			continue;

		int32_t nx = (int32_t)cx + dx[d];
		int32_t ny = (int32_t)cy + dy[d];
		if (nx < (int32_t)bx0 || ny < (int32_t)by0 || (uint32_t)nx > bx1 || (uint32_t)ny > by1)
			continue;

		talos_tile_t oldVal = grid[cz][ny][nx];
		if (oldVal == Empty_Tile)
			continue;

		if (TALOS_POPCOUNT(oldVal) == 1)
		{
			if (isMultiRule && (oldVal & dirMasks[d]) == 0)
			{
				backtrackChunkNeighborhood3D(grid, width, length, height, heap, (uint32_t)nx, (uint32_t)ny, cz, rng);
				*tail = 0;
				return;
			}
			continue;
		}

		talos_tile_t newVal = oldVal & dirMasks[d];
		if (newVal == 0)
		{
			if (isMultiRule)
			{
				talos_tile_t exact = exactCellValidMask3D(grid, width, length, height, (uint32_t)nx, (uint32_t)ny, cz) & dirMasks[d];
				if (exact != Empty_Tile)
				{
					newVal = exact;
				}
				else
				{
					backtrackChunkNeighborhood3D(grid, width, length, height, heap, (uint32_t)nx, (uint32_t)ny, cz, rng);
					*tail = 0;
					return;
				}
			}
			else
			{
				newVal = recoverCellCandidates3D(grid, width, length, height, (uint32_t)nx, (uint32_t)ny, cz);
			}
		}
		grid[cz][ny][nx] = newVal;

		if (newVal != oldVal)
		{
			if (TALOS_POPCOUNT(newVal) <= 1 || isMultiRule)
			{
				if (*tail < TALOS_UNIT_PROP_CAP)
					queue[(*tail)++] = (SolverUnitPoint3D){ (uint16_t)nx, (uint16_t)ny, (uint16_t)cz };
			}
			if (TALOS_POPCOUNT(newVal) > 1 && heap)
			{
				chunkHeapInsertOrUpdate3D(heap, grid, (uint32_t)nx, (uint32_t)ny, cz, rng);
			}
		}
	}

	// vertical propagation (U = z+1, D = z-1)
	if (height > 1 && vertMasks != NULL)
	{
		for (int vd = 0; vd < 2; vd++)
		{
			if (vertMasks[vd] == All_Possible_State)
				continue;

			int32_t nz = (vd == VERT_IDX_UP) ? ((int32_t)cz + 1) : ((int32_t)cz - 1);
			if (nz < 0 || (uint32_t)nz >= height)
				continue;

			talos_tile_t oldVal = grid[nz][cy][cx];
			if (oldVal == Empty_Tile)
				continue;

			if (TALOS_POPCOUNT(oldVal) == 1)
			{
				if (isMultiRule && (oldVal & vertMasks[vd]) == 0)
				{
					backtrackChunkNeighborhood3D(grid, width, length, height, heap, cx, cy, (uint32_t)nz, rng);
					*tail = 0;
					return;
				}
				continue;
			}

			talos_tile_t newVal = oldVal & vertMasks[vd];
			if (newVal == 0)
			{
				if (isMultiRule)
				{
					talos_tile_t exact = exactCellValidMask3D(grid, width, length, height, cx, cy, (uint32_t)nz) & vertMasks[vd];
					if (exact != Empty_Tile)
					{
						newVal = exact;
					}
					else
					{
						backtrackChunkNeighborhood3D(grid, width, length, height, heap, cx, cy, (uint32_t)nz, rng);
						*tail = 0;
						return;
					}
				}
				else
				{
					newVal = recoverCellCandidates3D(grid, width, length, height, cx, cy, (uint32_t)nz);
				}
			}
			grid[nz][cy][cx] = newVal;

			if (newVal != oldVal)
			{
				if (TALOS_POPCOUNT(newVal) <= 1 || isMultiRule)
				{
					if (*tail < TALOS_UNIT_PROP_CAP)
						queue[(*tail)++] = (SolverUnitPoint3D){ (uint16_t)cx, (uint16_t)cy, (uint16_t)nz };
				}
				if (TALOS_POPCOUNT(newVal) > 1 && heap)
				{
					chunkHeapInsertOrUpdate3D(heap, grid, cx, cy, (uint32_t)nz, rng);
				}
			}
		}
	}
}

void updateNeighboursChunk3D(
	talos_tile_t ***grid, uint32_t width, uint32_t length, uint32_t height,
	ChunkHeap3D *heap, uint32_t x, uint32_t y, uint32_t z, uint32_t *rng)
{
	SolverUnitPoint3D queue[TALOS_UNIT_PROP_CAP];
	int head = 0, tail = 0;
	propagateCellImmediate3D(grid, width, length, height, heap, x, y, z, rng, queue, &tail);

	while (head < tail)
	{
		SolverUnitPoint3D cur = queue[head++];
		propagateCellImmediate3D(grid, width, length, height, heap, cur.x, cur.y, cur.z, rng, queue, &tail);
	}
}

bool findBestSeedLocationChunk3D(
	talos_tile_t ***grid, uint32_t width, uint32_t length, uint32_t height,
	ChunkHeap3D *heap, uint32_t *outX, uint32_t *outY, uint32_t *outZ, uint32_t *rng)
{
	float minScore = 1e9f;
	bool found = false;

	uint32_t bx0 = heap ? heap->x0 : 0;
	uint32_t by0 = heap ? heap->y0 : 0;
	uint32_t bx1 = heap ? (heap->x0 + heap->cw - 1) : (width - 1);
	uint32_t by1 = heap ? (heap->y0 + heap->cl - 1) : (length - 1);

	for (uint32_t z = 0; z < height; z++)
	{
		for (uint32_t i = by0; i <= by1; i++)
		{
			for (uint32_t j = bx0; j <= bx1; j++)
			{
				talos_tile_t tile = grid[z][i][j];
				if (tile == Empty_Tile)
					continue;

				int popcount = TALOS_POPCOUNT(tile);
				if (popcount == 1)
					continue;

				if (popcount == 0)
				{
					grid[z][i][j] = recoverCellCandidates3D(grid, width, length, height, j, i, z);
					popcount = TALOS_POPCOUNT(grid[z][i][j]);
					if (grid[z][i][j] == Empty_Tile)
						continue;
					if (popcount == 1)
					{
						*outX = j;
						*outY = i;
						*outZ = z;
						return true;
					}
				}

				float noise = (float)((((z * length + i) * width + j) ^ nextRandom(rng)) & 0xFFFF) * 0.001f;
				if (noise < minScore)
				{
					minScore = noise;
					*outX = j;
					*outY = i;
					*outZ = z;
					found = true;
				}
			}
		}
	}

	return found;
}
