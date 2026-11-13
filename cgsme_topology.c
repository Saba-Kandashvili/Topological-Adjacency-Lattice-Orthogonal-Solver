#include "cgsme_topology.h"
#include "tiles.h"
#include "cgsme_utils.h"
#include "threadRandom.h"

// open port dirIdx (0=N, 1=E, 2=S, 3=W) on packed cell (x, y) using CGSME_OPEN_LUT
void openWallPacked(cgsme_tile_t **grid, uint32_t x, uint32_t y, uint8_t dirIdx)
{
	CGSME_PROFILE_FUNC();
	if (grid[y][x] == CGSME_PACKED_VOID)
		return;

	cgsme_tile_t visited = grid[y][x] & CGSME_VISITED_BIT;
	uint8_t idx = (uint8_t)(grid[y][x] & CGSME_TILE_INDEX_MASK);

	// if this port is already open, keep the exact tile index (preserves Special_X stairs, ...)
	if (CGSME_BIT(idx) & CGSME_HAS_PORT_MASK[dirIdx])
		return;

	uint8_t newIdx = CGSME_OPEN_LUT[idx][dirIdx];
	grid[y][x] = visited | (cgsme_tile_t)newIdx;
}

// COMPRESS bitmasks -> compact tile indices [0 .. CGSME_NUM_TILES-1], leaving CGSME_VISITED_BIT = 0
void findConnectedRegionsInPlace(cgsme_tile_t **grid, uint32_t width, uint32_t length)
{
	CGSME_PROFILE_FUNC();
	for (uint32_t i = 0; i < length; i++)
	{
		for (uint32_t j = 0; j < width; j++)
		{
			if (grid[i][j] == Empty_Tile)
				grid[i][j] = CGSME_PACKED_VOID;
			else
				grid[i][j] = (cgsme_tile_t)CGSME_CTZ(grid[i][j]);
		}
	}
}

// iterative BFS starting at (startX, startY):
// - sets CGSME_VISITED_BIT on all tiles bidirectionally reachable from the start cell
// - pushes every boundary edge pointing at an unvisited non-void neighbor onto `frontier`
static void ariadneExpandFrom(
	cgsme_tile_t **grid, uint32_t width, uint32_t length,
	uint32_t startX, uint32_t startY,
	TopoNode *bfs_queue,
	AriadneEdge *frontier, uint32_t *frontier_count, uint32_t max_frontier)
{
	int head = 0, tail = 0;
	grid[startY][startX] |= CGSME_VISITED_BIT;
	bfs_queue[tail++] = (TopoNode){ startX, startY };

	static const int32_t dx[4] = {  0,  1,  0, -1 }; // N, E, S, W
	static const int32_t dy[4] = { -1,  0,  1,  0 };

	while (head < tail)
	{
		TopoNode c = bfs_queue[head++];
		uint8_t idx = (uint8_t)(grid[c.y][c.x] & CGSME_TILE_INDEX_MASK);
		cgsme_tile_t mask = CGSME_BIT(idx);

		for (uint8_t d = 0; d < 4; d++)
		{
			int32_t nx = (int32_t)c.x + dx[d];
			int32_t ny = (int32_t)c.y + dy[d];
			if (nx < 0 || ny < 0 || (uint32_t)nx >= width || (uint32_t)ny >= length)
				continue;

			cgsme_tile_t nPacked = grid[ny][nx];
			if (nPacked == CGSME_PACKED_VOID || (nPacked & CGSME_VISITED_BIT))
				continue;

			uint8_t nIdx = (uint8_t)(nPacked & CGSME_TILE_INDEX_MASK);
			cgsme_tile_t nMask = CGSME_BIT(nIdx);
			uint8_t oppDir = DIR_OPPOSITE(d);

			// bidirectional port check: only already connected if BOTH sides open toward each other
			if ((mask & CGSME_HAS_PORT_MASK[d]) && (nMask & CGSME_HAS_PORT_MASK[oppDir]))
			{
				grid[ny][nx] |= CGSME_VISITED_BIT;
				bfs_queue[tail++] = (TopoNode){ (uint32_t)nx, (uint32_t)ny };
			}
			else if (*frontier_count < max_frontier)
			{
				// neighbor belongs to an unvisited region across a closed/one-way boundary
				frontier[(*frontier_count)++] = (AriadneEdge){ c.x, c.y, d };
			}
		}
	}
}

// 1 bit "Ariadne" quotient graph randomized prim welder (damna nother acronym? QGRPW)
// welds arbitrarily many regions using only 1 status bit (CGSME_VISITED_BIT)
// supporting 8/16/32/64-bit tile containers without any max region ID cap
void germanWelderInPlace(cgsme_tile_t **grid, uint32_t width, uint32_t length, uint32_t *rng)
{
	CGSME_PROFILE_FUNC();

	uint32_t total_cells = width * length;
	// each unordered adjacent cell pair can push at most once (when the first endpoint becomes visited)
	uint32_t max_frontier = total_cells * 2;

	TopoNode *bfs_queue = malloc(sizeof(TopoNode) * total_cells);
	AriadneEdge *frontier = malloc(sizeof(AriadneEdge) * max_frontier);
	if (!bfs_queue || !frontier)
	{
		free(bfs_queue);
		free(frontier);
		return;
	}

	uint32_t frontier_count = 0;
	static const int32_t dx[4] = {  0,  1,  0, -1 }; // N, E, S, W
	static const int32_t dy[4] = { -1,  0,  1,  0 };

	for (uint32_t y = 0; y < length; y++)
	{
		for (uint32_t x = 0; x < width; x++)
		{
			if (grid[y][x] == CGSME_PACKED_VOID || (grid[y][x] & CGSME_VISITED_BIT))
				continue;

			// Seed the Ariadne tree from this unvisited component
			ariadneExpandFrom(grid, width, length, x, y, bfs_queue, frontier, &frontier_count, max_frontier);

			while (frontier_count > 0)
			{
				uint32_t pick = nextRandom(rng) % frontier_count;
				AriadneEdge e = frontier[pick];
				frontier[pick] = frontier[--frontier_count];

				uint32_t nx = (uint32_t)((int32_t)e.x + dx[e.dir]);
				uint32_t ny = (uint32_t)((int32_t)e.y + dy[e.dir]);

				// stale check: neighbor was already absorbed via another bridge
				if (grid[ny][nx] == CGSME_PACKED_VOID || (grid[ny][nx] & CGSME_VISITED_BIT))
					continue;

				// weld both sides open using CGSME_OPEN_LUT
				openWallPacked(grid, e.x, e.y, e.dir);
				openWallPacked(grid, nx, ny, DIR_OPPOSITE(e.dir));

				// Absorb the newly connected island into the visited set
				ariadneExpandFrom(grid, width, length, nx, ny, bfs_queue, frontier, &frontier_count, max_frontier);
			}
		}
	}

	free(bfs_queue);
	free(frontier);
}

void sealMazeEdges(cgsme_tile_t **gridLayer, uint32_t width, uint32_t length)
{
	CGSME_PROFILE_FUNC();
	for (uint32_t y = 0; y < length; y++)
	{
		for (uint32_t x = 0; x < width; x++)
		{
			if (gridLayer[y][x] != Empty_Tile)
				continue;

			bool open_dir[4] = { false, false, false, false };

			// check north neighbor (y-1): does it open South toward us?
			if (y > 0 && gridLayer[y - 1][x] != Empty_Tile && (gridLayer[y - 1][x] & CGSME_HAS_PORT_MASK[DIR_IDX_S]))
				open_dir[DIR_IDX_N] = true;

			// 2. Check East Neighbor (x+1): does it open West toward us?
			if (x < width - 1 && gridLayer[y][x + 1] != Empty_Tile && (gridLayer[y][x + 1] & CGSME_HAS_PORT_MASK[DIR_IDX_W]))
				open_dir[DIR_IDX_E] = true;

			// 3. Check South Neighbor (y+1): does it open North toward us?
			if (y < length - 1 && gridLayer[y + 1][x] != Empty_Tile && (gridLayer[y + 1][x] & CGSME_HAS_PORT_MASK[DIR_IDX_N]))
				open_dir[DIR_IDX_S] = true;

			// 4. Check West Neighbor (x-1): does it open East toward us?
			if (x > 0 && gridLayer[y][x - 1] != Empty_Tile && (gridLayer[y][x - 1] & CGSME_HAS_PORT_MASK[DIR_IDX_E]))
				open_dir[DIR_IDX_W] = true;

			if (open_dir[0] || open_dir[1] || open_dir[2] || open_dir[3])
			{
				cgsme_tile_t candidates = All_Possible_State;
				for (int d = 0; d < 4; d++)
				{
					if (open_dir[d])
						candidates &= CGSME_HAS_PORT_MASK[d];
					else
						candidates &= ~CGSME_HAS_PORT_MASK[d];
				}
				if (candidates != Empty_Tile)
					gridLayer[y][x] = CGSME_BIT(CGSME_CTZ(candidates));
			}
		}
	}
}

// Helper: close port `dir` on *cell if it is open, using CGSME_CLOSE_LUT
static inline void closeTilePort(cgsme_tile_t *cell, uint8_t dir)
{
	if (*cell != Empty_Tile && (*cell & CGSME_HAS_PORT_MASK[dir]))
	{
		uint8_t idx = (uint8_t)CGSME_CTZ(*cell);
		uint8_t nextIdx = CGSME_CLOSE_LUT[idx][dir];
		*cell = (nextIdx == 0xFF) ? Empty_Tile : CGSME_BIT(nextIdx);
	}
}

void fixupEdges(cgsme_tile_t **gridLayer, uint32_t width, uint32_t length)
{
	CGSME_PROFILE_FUNC();
	// Top row (y = 0): close NORTH; Bottom row (y = length - 1): close SOUTH
	for (uint32_t x = 0; x < width; x++)
	{
		closeTilePort(&gridLayer[0][x], DIR_IDX_N);
		closeTilePort(&gridLayer[length - 1][x], DIR_IDX_S);
	}

	// Left column (x = 0): close WEST; Right column (x = width - 1): close EAST
	for (uint32_t y = 0; y < length; y++)
	{
		closeTilePort(&gridLayer[y][0], DIR_IDX_W);
		closeTilePort(&gridLayer[y][width - 1], DIR_IDX_E);
	}
}
