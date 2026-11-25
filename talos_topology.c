#include "talos_topology.h"
#include "tiles.h"
#include "talos_utils.h"
#include "threadRandom.h"
#include <string.h>

static const int32_t TOPO_DX[4] = {  0,  1,  0, -1 }; // N, E, S, W
static const int32_t TOPO_DY[4] = { -1,  0,  1,  0 };

static inline talos_tile_t hasAnyOpenPortMask(void)
{
	return TALOS_HAS_PORT_MASK[DIR_IDX_N] | TALOS_HAS_PORT_MASK[DIR_IDX_E] |
		   TALOS_HAS_PORT_MASK[DIR_IDX_S] | TALOS_HAS_PORT_MASK[DIR_IDX_W];
}

static inline uint8_t popcount4(uint8_t mask)
{
	static const uint8_t tbl[16] = { 0,1,1,2, 1,2,2,3, 1,2,2,3, 2,3,3,4 };
	return tbl[mask & 0xFu];
}

static inline uint8_t portBits4OfIndex(uint8_t idx)
{
	talos_tile_t bit = TALOS_BIT(idx);
	return (uint8_t)(
		((bit & TALOS_HAS_PORT_MASK[DIR_IDX_N]) ? (1u << DIR_IDX_N) : 0u) |
		((bit & TALOS_HAS_PORT_MASK[DIR_IDX_E]) ? (1u << DIR_IDX_E) : 0u) |
		((bit & TALOS_HAS_PORT_MASK[DIR_IDX_S]) ? (1u << DIR_IDX_S) : 0u) |
		((bit & TALOS_HAS_PORT_MASK[DIR_IDX_W]) ? (1u << DIR_IDX_W) : 0u));
}

static inline uint8_t voidOobDirsUnpacked(talos_tile_t **grid, uint32_t width, uint32_t length, uint32_t x, uint32_t y)
{
	uint8_t voidDirs = 0;
	for (uint8_t d = 0; d < 4; d++)
	{
		int32_t nx = (int32_t)x + TOPO_DX[d];
		int32_t ny = (int32_t)y + TOPO_DY[d];
		if (nx < 0 || ny < 0 || (uint32_t)nx >= width || (uint32_t)ny >= length || grid[ny][nx] == Empty_Tile)
			voidDirs |= (uint8_t)(1u << d);
	}
	return voidDirs;
}

static inline uint8_t voidOobDirsPacked(talos_tile_t **grid, uint32_t width, uint32_t length, uint32_t x, uint32_t y)
{
	uint8_t voidDirs = 0;
	for (uint8_t d = 0; d < 4; d++)
	{
		int32_t nx = (int32_t)x + TOPO_DX[d];
		int32_t ny = (int32_t)y + TOPO_DY[d];
		if (nx < 0 || ny < 0 || (uint32_t)nx >= width || (uint32_t)ny >= length || grid[ny][nx] == TALOS_PACKED_VOID)
			voidDirs |= (uint8_t)(1u << d);
	}
	return voidDirs;
}

static inline uint8_t bidirOpenDirsUnpacked(talos_tile_t **grid, uint32_t width, uint32_t length, uint32_t x, uint32_t y)
{
	talos_tile_t cell = grid[y][x];
	if (cell == Empty_Tile)
		return 0;

	uint8_t bidir = 0;
	for (uint8_t d = 0; d < 4; d++)
	{
		if (!(cell & TALOS_HAS_PORT_MASK[d]))
			continue;
		int32_t nx = (int32_t)x + TOPO_DX[d];
		int32_t ny = (int32_t)y + TOPO_DY[d];
		if (nx < 0 || ny < 0 || (uint32_t)nx >= width || (uint32_t)ny >= length)
			continue;
		talos_tile_t nb = grid[ny][nx];
		if (nb != Empty_Tile && (nb & TALOS_HAS_PORT_MASK[DIR_OPPOSITE(d)]))
			bidir |= (uint8_t)(1u << d);
	}
	return bidir;
}

// find void-safe tile that opens openDir while keeping current ports (returns 0xFF if impossible)
static uint8_t findSafeOpenReplacement(uint8_t curIdx, uint8_t openDir, uint8_t voidDirs4, int *outExtraOpened)
{
	uint8_t curPorts = portBits4OfIndex(curIdx);
	if (curPorts & (1u << openDir))
	{
		*outExtraOpened = 0;
		return curIdx;
	}

	if (voidDirs4 & (1u << openDir))
		return 0xFF;

	uint8_t lutIdx = TALOS_OPEN_LUT[curIdx][openDir];
	uint8_t lutPorts = portBits4OfIndex(lutIdx);
	if ((lutPorts & (1u << openDir)) && (lutPorts & voidDirs4) == 0)
	{
		*outExtraOpened = 0;
		return lutIdx;
	}

	uint8_t bestIdx = 0xFF;
	int bestExtra = 99;
	for (int t = 0; t < TALOS_NUM_TILES; t++)
	{
		if (!TALOS_CATEGORY_NATURAL[TALOS_BIT_TO_CATEGORY[t]])
			continue;

		uint8_t tPorts = portBits4OfIndex((uint8_t)t);
		if (!(tPorts & (1u << openDir)))
			continue;
		if (tPorts & voidDirs4)
			continue;
		if ((tPorts & curPorts) != curPorts)
			continue;

		int extra = (int)popcount4((uint8_t)(tPorts & ~curPorts));
		if (extra < bestExtra)
		{
			bestExtra = extra;
			bestIdx = (uint8_t)t;
		}
	}

	if (bestIdx != 0xFF)
		*outExtraOpened = bestExtra;
	return bestIdx;
}

// find void-safe tile that closes closeDir (returns 0xFF if cell has to become void)
static uint8_t findSafeCloseReplacement(
	talos_tile_t **grid, uint32_t width, uint32_t length,
	uint32_t x, uint32_t y, uint8_t curIdx, uint8_t closeDir)
{
	uint8_t voidDirs = voidOobDirsUnpacked(grid, width, length, x, y);
	uint8_t lutIdx = TALOS_CLOSE_LUT[curIdx][closeDir];
	if (lutIdx != 0xFF && (portBits4OfIndex(lutIdx) & voidDirs) == 0)
		return lutIdx;

	uint8_t curPorts = portBits4OfIndex(curIdx);
	uint8_t bidirDirs = bidirOpenDirsUnpacked(grid, width, length, x, y);
	uint8_t bestIdx = 0xFF;
	int bestScore = -1;

	for (int t = 0; t < TALOS_NUM_TILES; t++)
	{
		uint8_t tPorts = portBits4OfIndex((uint8_t)t);
		if (tPorts & (1u << closeDir))
			continue;
		if (tPorts & voidDirs)
			continue;
		if ((tPorts & ~curPorts) != 0 || tPorts == curPorts)
			continue;

		int keptBidir = (int)popcount4((uint8_t)(tPorts & bidirDirs));
		int keptTotal = (int)popcount4(tPorts);
		int isNatural = TALOS_CATEGORY_NATURAL[TALOS_BIT_TO_CATEGORY[t]] ? 1 : 0;
		int score = keptBidir * 16 + keptTotal * 2 + isNatural;
		if (score > bestScore)
		{
			bestScore = score;
			bestIdx = (uint8_t)t;
		}
	}

	return bestIdx;
}

// open port dirIdx (0=N, 1=E, 2=S, 3=W) on packed cell (x, y) using TALOS_OPEN_LUT
void openWallPacked(talos_tile_t **grid, uint32_t x, uint32_t y, uint8_t dirIdx)
{
	TALOS_PROFILE_FUNC();
	if (grid[y][x] == TALOS_PACKED_VOID)
		return;

	talos_tile_t visited = grid[y][x] & TALOS_VISITED_BIT;
	uint8_t idx = (uint8_t)(grid[y][x] & TALOS_TILE_INDEX_MASK);

	// if this port is already open, keep the exact tile index (preserves Special_X stairs, ...)
	if (TALOS_BIT(idx) & TALOS_HAS_PORT_MASK[dirIdx])
		return;

	uint8_t newIdx = TALOS_OPEN_LUT[idx][dirIdx];
	grid[y][x] = visited | (talos_tile_t)newIdx;
}

// COMPRESS bitmasks -> compact tile indices [0 .. TALOS_NUM_TILES-1], leaving TALOS_VISITED_BIT = 0
void findConnectedRegionsInPlace(talos_tile_t **grid, uint32_t width, uint32_t length)
{
	TALOS_PROFILE_FUNC();
	for (uint32_t i = 0; i < length; i++)
	{
		for (uint32_t j = 0; j < width; j++)
		{
			if (grid[i][j] == Empty_Tile)
				grid[i][j] = TALOS_PACKED_VOID;
			else
				grid[i][j] = (talos_tile_t)TALOS_CTZ(grid[i][j]);
		}
	}
}

// bfs from (startX, startY), marks visited and collects boundary edges to unvisited regions
static void ariadneExpandFrom(
	talos_tile_t **grid, uint32_t width, uint32_t length,
	uint32_t startX, uint32_t startY,
	TopoNode *bfs_queue,
	AriadneEdge *frontier, uint32_t *frontier_count, uint32_t max_frontier)
{
	int head = 0, tail = 0;
	if (!(grid[startY][startX] & TALOS_VISITED_BIT))
	{
		grid[startY][startX] |= TALOS_VISITED_BIT;
		bfs_queue[tail++] = (TopoNode){ startX, startY };
	}
	else
	{
		// recheck outward edges if this node gained new ports
		bfs_queue[tail++] = (TopoNode){ startX, startY };
	}

	while (head < tail)
	{
		TopoNode c = bfs_queue[head++];
		uint8_t idx = (uint8_t)(grid[c.y][c.x] & TALOS_TILE_INDEX_MASK);
		talos_tile_t mask = TALOS_BIT(idx);

		for (uint8_t d = 0; d < 4; d++)
		{
			int32_t nx = (int32_t)c.x + TOPO_DX[d];
			int32_t ny = (int32_t)c.y + TOPO_DY[d];
			if (nx < 0 || ny < 0 || (uint32_t)nx >= width || (uint32_t)ny >= length)
				continue;

			talos_tile_t nPacked = grid[ny][nx];
			if (nPacked == TALOS_PACKED_VOID || (nPacked & TALOS_VISITED_BIT))
				continue;

			uint8_t nIdx = (uint8_t)(nPacked & TALOS_TILE_INDEX_MASK);
			talos_tile_t nMask = TALOS_BIT(nIdx);
			uint8_t oppDir = DIR_OPPOSITE(d);

			// bidirectional port check: only already connected if BOTH sides open toward each other
			if ((mask & TALOS_HAS_PORT_MASK[d]) && (nMask & TALOS_HAS_PORT_MASK[oppDir]))
			{
				grid[ny][nx] |= TALOS_VISITED_BIT;
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

// score a frontier bridge: 1 = exact open, 2 = clean superset, 3 = superset, 0 = cant bridge
static int evaluateBridgePacked(
	talos_tile_t **grid, uint32_t width, uint32_t length,
	const AriadneEdge *e, uint8_t *outIdxA, uint8_t *outIdxB)
{
	uint32_t nx = (uint32_t)((int32_t)e->x + TOPO_DX[e->dir]);
	uint32_t ny = (uint32_t)((int32_t)e->y + TOPO_DY[e->dir]);
	uint8_t oppDir = DIR_OPPOSITE(e->dir);

	if (grid[e->y][e->x] == TALOS_PACKED_VOID || grid[ny][nx] == TALOS_PACKED_VOID)
		return 0;

	uint8_t idxA = (uint8_t)(grid[e->y][e->x] & TALOS_TILE_INDEX_MASK);
	uint8_t idxB = (uint8_t)(grid[ny][nx] & TALOS_TILE_INDEX_MASK);

	uint8_t newA = 0xFF, newB = 0xFF;
	int extraA = 0, extraB = 0;

	// fast path when OPEN_LUT adds only the single needed port
	uint8_t portsA = portBits4OfIndex(idxA);
	if (portsA & (1u << e->dir))
	{
		newA = idxA;
	}
	else
	{
		uint8_t lutA = TALOS_OPEN_LUT[idxA][e->dir];
		if (TALOS_CATEGORY_NATURAL[TALOS_BIT_TO_CATEGORY[lutA]] &&
			portBits4OfIndex(lutA) == (uint8_t)(portsA | (1u << e->dir)))
			newA = lutA;
		else
			newA = findSafeOpenReplacement(idxA, e->dir, voidOobDirsPacked(grid, width, length, e->x, e->y), &extraA);
	}

	uint8_t portsB = portBits4OfIndex(idxB);
	if (portsB & (1u << oppDir))
	{
		newB = idxB;
	}
	else
	{
		uint8_t lutB = TALOS_OPEN_LUT[idxB][oppDir];
		if (TALOS_CATEGORY_NATURAL[TALOS_BIT_TO_CATEGORY[lutB]] &&
			portBits4OfIndex(lutB) == (uint8_t)(portsB | (1u << oppDir)))
			newB = lutB;
		else
			newB = findSafeOpenReplacement(idxB, oppDir, voidOobDirsPacked(grid, width, length, nx, ny), &extraB);
	}

	if (newA == 0xFF || newB == 0xFF)
		return 0;

	*outIdxA = newA;
	*outIdxB = newB;

	if (extraA == 0 && extraB == 0)
		return 1;

	// check if any extra opened ports can match their neighbors cleanly
	const uint32_t px[2] = { e->x, nx };
	const uint32_t py[2] = { e->y, ny };
	const uint8_t oldIdx[2] = { idxA, idxB };
	const uint8_t repIdx[2] = { newA, newB };
	const uint8_t bridgeDir[2] = { e->dir, oppDir };

	bool allExtraClean = true;
	for (int s = 0; s < 2 && allExtraClean; s++)
	{
		uint8_t addedPorts = (uint8_t)(portBits4OfIndex(repIdx[s]) & ~portBits4OfIndex(oldIdx[s]));
		for (uint8_t d = 0; d < 4; d++)
		{
			if (d == bridgeDir[s] || !(addedPorts & (1u << d)))
				continue;

			int32_t wx = (int32_t)px[s] + TOPO_DX[d];
			int32_t wy = (int32_t)py[s] + TOPO_DY[d];
			if (wx < 0 || wy < 0 || (uint32_t)wx >= width || (uint32_t)wy >= length || grid[wy][wx] == TALOS_PACKED_VOID)
			{
				allExtraClean = false;
				break;
			}

			uint8_t wIdx = (uint8_t)(grid[wy][wx] & TALOS_TILE_INDEX_MASK);
			int wExtra = 0;
			uint8_t wRep = findSafeOpenReplacement(
				wIdx, DIR_OPPOSITE(d),
				voidOobDirsPacked(grid, width, length, (uint32_t)wx, (uint32_t)wy), &wExtra);
			if (wRep == 0xFF || wExtra > 0)
			{
				allExtraClean = false;
				break;
			}
		}
	}

	return allExtraClean ? 2 : 3;
}

static void openExtraNeighborsSafePacked(
	talos_tile_t **grid, uint32_t width, uint32_t length,
	uint32_t x, uint32_t y, uint8_t oldIdx, uint8_t newIdx, uint8_t skipDir)
{
	uint8_t added = (uint8_t)(portBits4OfIndex(newIdx) & ~portBits4OfIndex(oldIdx));
	for (uint8_t d = 0; d < 4; d++)
	{
		if (d == skipDir || !(added & (1u << d)))
			continue;

		int32_t wx = (int32_t)x + TOPO_DX[d];
		int32_t wy = (int32_t)y + TOPO_DY[d];
		if (wx < 0 || wy < 0 || (uint32_t)wx >= width || (uint32_t)wy >= length || grid[wy][wx] == TALOS_PACKED_VOID)
			continue;

		uint8_t opp = DIR_OPPOSITE(d);
		uint8_t wIdx = (uint8_t)(grid[wy][wx] & TALOS_TILE_INDEX_MASK);
		int extra = 0;
		uint8_t wRep = findSafeOpenReplacement(
			wIdx, opp, voidOobDirsPacked(grid, width, length, (uint32_t)wx, (uint32_t)wy), &extra);
		if (wRep != 0xFF)
		{
			talos_tile_t vis = grid[wy][wx] & TALOS_VISITED_BIT;
			grid[wy][wx] = vis | (talos_tile_t)wRep;
		}
	}
}

// 1 bit "Ariadne" quotient graph randomized prim welder (QGRPW)
// Welds all adjacent reachable components using 1 status bit (TALOS_VISITED_BIT).
void germanWelderExInPlace(
	talos_tile_t **grid, uint32_t width, uint32_t length, uint32_t *rng,
	bool *outAllConnected, bool *outUsedSuperset)
{
	TALOS_PROFILE_FUNC();
	if (outAllConnected)
		*outAllConnected = true;
	if (outUsedSuperset)
		*outUsedSuperset = false;

	if (hasAnyOpenPortMask() == Empty_Tile)
		return;

	uint32_t total_cells = width * length;
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
	uint32_t seedTrees = 0;
	bool unbridgeable = false;
	bool usedSuperset = false;

	for (uint32_t y = 0; y < length; y++)
	{
		for (uint32_t x = 0; x < width; x++)
		{
			if (grid[y][x] == TALOS_PACKED_VOID || (grid[y][x] & TALOS_VISITED_BIT))
				continue;

			// seed from unvisited component
			seedTrees++;
			ariadneExpandFrom(grid, width, length, x, y, bfs_queue, frontier, &frontier_count, max_frontier);

			while (frontier_count > 0)
			{
				// try random frontier edge at tier 1 first
				uint32_t pick = nextRandom(rng) % frontier_count;
				AriadneEdge chosen = frontier[pick];
				uint32_t nx = (uint32_t)((int32_t)chosen.x + TOPO_DX[chosen.dir]);
				uint32_t ny = (uint32_t)((int32_t)chosen.y + TOPO_DY[chosen.dir]);

				if (grid[ny][nx] == TALOS_PACKED_VOID || (grid[ny][nx] & TALOS_VISITED_BIT))
				{
					frontier[pick] = frontier[--frontier_count];
					continue;
				}

				uint8_t newA = 0, newB = 0;
				int tier = evaluateBridgePacked(grid, width, length, &chosen, &newA, &newB);
				if (tier == 1)
				{
					frontier[pick] = frontier[--frontier_count];
				}
				else
				{
					// fallback: compact stale edges and pick best tier (1 -> 2 -> 3)
					int bestTier = 99;
					uint32_t matchCount = 0;
					uint32_t bestIdx = 0;
					uint8_t bestA = 0, bestB = 0;
					uint32_t write = 0;

					for (uint32_t i = 0; i < frontier_count; i++)
					{
						AriadneEdge cur = frontier[i];
						uint32_t cnx = (uint32_t)((int32_t)cur.x + TOPO_DX[cur.dir]);
						uint32_t cny = (uint32_t)((int32_t)cur.y + TOPO_DY[cur.dir]);
						if (grid[cny][cnx] == TALOS_PACKED_VOID || (grid[cny][cnx] & TALOS_VISITED_BIT))
							continue;

						frontier[write] = cur;
						uint8_t candA = 0, candB = 0;
						int cTier = evaluateBridgePacked(grid, width, length, &cur, &candA, &candB);
						if (cTier > 0)
						{
							if (cTier < bestTier)
							{
								bestTier = cTier;
								matchCount = 1;
								bestIdx = write;
								bestA = candA;
								bestB = candB;
							}
							else if (cTier == bestTier)
							{
								matchCount++;
								if ((nextRandom(rng) % matchCount) == 0)
								{
									bestIdx = write;
									bestA = candA;
									bestB = candB;
								}
							}
						}
						write++;
					}
					frontier_count = write;

					if (bestTier > 3)
					{
						// nothing left we can weld on this component
						unbridgeable = true;
						frontier_count = 0;
						break;
					}

					chosen = frontier[bestIdx];
					frontier[bestIdx] = frontier[--frontier_count];
					nx = (uint32_t)((int32_t)chosen.x + TOPO_DX[chosen.dir]);
					ny = (uint32_t)((int32_t)chosen.y + TOPO_DY[chosen.dir]);
					newA = bestA;
					newB = bestB;
					tier = bestTier;
				}

				uint8_t oldA = (uint8_t)(grid[chosen.y][chosen.x] & TALOS_TILE_INDEX_MASK);
				uint8_t oldB = (uint8_t)(grid[ny][nx] & TALOS_TILE_INDEX_MASK);

				grid[chosen.y][chosen.x] = (grid[chosen.y][chosen.x] & TALOS_VISITED_BIT) | (talos_tile_t)newA;
				grid[ny][nx] = (grid[ny][nx] & TALOS_VISITED_BIT) | (talos_tile_t)newB;

				if (tier >= 2)
				{
					usedSuperset = true;
					openExtraNeighborsSafePacked(grid, width, length, chosen.x, chosen.y, oldA, newA, chosen.dir);
					openExtraNeighborsSafePacked(grid, width, length, nx, ny, oldB, newB, DIR_OPPOSITE(chosen.dir));
					// re-expand from A in case an extra port hits an unvisited cell
					ariadneExpandFrom(grid, width, length, chosen.x, chosen.y, bfs_queue, frontier, &frontier_count, max_frontier);
				}

				if (!(grid[ny][nx] & TALOS_VISITED_BIT))
					ariadneExpandFrom(grid, width, length, nx, ny, bfs_queue, frontier, &frontier_count, max_frontier);
			}
		}
	}

	if (outAllConnected)
		*outAllConnected = (seedTrees <= 1 && !unbridgeable);
	if (outUsedSuperset)
		*outUsedSuperset = usedSuperset;

	free(bfs_queue);
	free(frontier);
}

void germanWelderInPlace(talos_tile_t **grid, uint32_t width, uint32_t length, uint32_t *rng)
{
	germanWelderExInPlace(grid, width, length, rng, NULL, NULL);
}

void sealMazeEdges(talos_tile_t **gridLayer, uint32_t width, uint32_t length)
{
	TALOS_PROFILE_FUNC();
	if (hasAnyOpenPortMask() == Empty_Tile)
		return;

	for (uint32_t y = 0; y < length; y++)
	{
		for (uint32_t x = 0; x < width; x++)
		{
			if (gridLayer[y][x] != Empty_Tile)
				continue;

			bool open_dir[4] = { false, false, false, false };

			// check north neighbor (y-1): does it open South toward us?
			if (y > 0 && gridLayer[y - 1][x] != Empty_Tile && (gridLayer[y - 1][x] & TALOS_HAS_PORT_MASK[DIR_IDX_S]))
				open_dir[DIR_IDX_N] = true;

			// check east neighbor (x+1): does it open West toward us?
			if (x + 1 < width && gridLayer[y][x + 1] != Empty_Tile && (gridLayer[y][x + 1] & TALOS_HAS_PORT_MASK[DIR_IDX_W]))
				open_dir[DIR_IDX_E] = true;

			// check south neighbor (y+1): does it open North toward us?
			if (y + 1 < length && gridLayer[y + 1][x] != Empty_Tile && (gridLayer[y + 1][x] & TALOS_HAS_PORT_MASK[DIR_IDX_N]))
				open_dir[DIR_IDX_S] = true;

			// check west neighbor (x-1): does it open East toward us?
			if (x > 0 && gridLayer[y][x - 1] != Empty_Tile && (gridLayer[y][x - 1] & TALOS_HAS_PORT_MASK[DIR_IDX_E]))
				open_dir[DIR_IDX_W] = true;

			if (open_dir[0] || open_dir[1] || open_dir[2] || open_dir[3])
			{
				talos_tile_t candidates = All_Possible_State;
				for (int d = 0; d < 4; d++)
				{
					if (open_dir[d])
						candidates &= TALOS_HAS_PORT_MASK[d];
					else
						candidates &= ~TALOS_HAS_PORT_MASK[d];
				}
				if (candidates != Empty_Tile)
					gridLayer[y][x] = TALOS_BIT(TALOS_CTZ(candidates));
			}
		}
	}
}

void fixupEdges(talos_tile_t **gridLayer, uint32_t width, uint32_t length)
{
	TALOS_PROFILE_FUNC();
	if (hasAnyOpenPortMask() == Empty_Tile)
		return;

	for (uint32_t x = 0; x < width; x++)
	{
		if (gridLayer[0][x] != Empty_Tile && (gridLayer[0][x] & TALOS_HAS_PORT_MASK[DIR_IDX_N]))
		{
			uint8_t nextIdx = findSafeCloseReplacement(gridLayer, width, length, x, 0, (uint8_t)TALOS_CTZ(gridLayer[0][x]), DIR_IDX_N);
			gridLayer[0][x] = (nextIdx == 0xFF) ? Empty_Tile : TALOS_BIT(nextIdx);
		}
		if (gridLayer[length - 1][x] != Empty_Tile && (gridLayer[length - 1][x] & TALOS_HAS_PORT_MASK[DIR_IDX_S]))
		{
			uint8_t nextIdx = findSafeCloseReplacement(gridLayer, width, length, x, length - 1, (uint8_t)TALOS_CTZ(gridLayer[length - 1][x]), DIR_IDX_S);
			gridLayer[length - 1][x] = (nextIdx == 0xFF) ? Empty_Tile : TALOS_BIT(nextIdx);
		}
	}

	for (uint32_t y = 0; y < length; y++)
	{
		if (gridLayer[y][0] != Empty_Tile && (gridLayer[y][0] & TALOS_HAS_PORT_MASK[DIR_IDX_W]))
		{
			uint8_t nextIdx = findSafeCloseReplacement(gridLayer, width, length, 0, y, (uint8_t)TALOS_CTZ(gridLayer[y][0]), DIR_IDX_W);
			gridLayer[y][0] = (nextIdx == 0xFF) ? Empty_Tile : TALOS_BIT(nextIdx);
		}
		if (gridLayer[y][width - 1] != Empty_Tile && (gridLayer[y][width - 1] & TALOS_HAS_PORT_MASK[DIR_IDX_E]))
		{
			uint8_t nextIdx = findSafeCloseReplacement(gridLayer, width, length, width - 1, y, (uint8_t)TALOS_CTZ(gridLayer[y][width - 1]), DIR_IDX_E);
			gridLayer[y][width - 1] = (nextIdx == 0xFF) ? Empty_Tile : TALOS_BIT(nextIdx);
		}
	}
}

// shortest-path edge-flip repair for mismatches and void leaks without deleting valid tiles
static bool healMismatchesByPathFlip(talos_tile_t **gridLayer, uint32_t width, uint32_t length)
{
	uint8_t maskToTile[16];
	for (int m = 0; m < 16; m++)
		maskToTile[m] = 0xFF;
	for (int t = 0; t < TALOS_NUM_TILES; t++)
	{
		if (TALOS_CATEGORY_NATURAL[TALOS_BIT_TO_CATEGORY[t]])
		{
			uint8_t p = portBits4OfIndex((uint8_t)t);
			if (maskToTile[p] == 0xFF)
				maskToTile[p] = (uint8_t)t;
		}
	}

	uint32_t totalCells = width * length;
	uint8_t *ports = calloc(totalCells, sizeof(uint8_t));
	uint8_t *locked = calloc(totalCells, sizeof(uint8_t));
	uint8_t *vis = malloc((size_t)totalCells * 4);
	int32_t *prevState = malloc(sizeof(int32_t) * totalCells * 4);
	int32_t *bfsQ = malloc(sizeof(int32_t) * totalCells * 4);
	if (!ports || !locked || !vis || !prevState || !bfsQ)
	{
		free(ports);
		free(locked);
		free(vis);
		free(prevState);
		free(bfsQ);
		return false;
	}

	bool changed = false;
	uint8_t *origPorts = malloc(totalCells);
	if (!origPorts)
	{
		free(ports); free(locked); free(vis); free(prevState); free(bfsQ);
		return false;
	}

	for (uint32_t y = 0; y < length; y++)
	{
		for (uint32_t x = 0; x < width; x++)
		{
			uint32_t flat = y * width + x;
			talos_tile_t cell = gridLayer[y][x];
			if (cell == Empty_Tile)
			{
				locked[flat] = 1;
				ports[flat] = 0;
				origPorts[flat] = 0;
				continue;
			}
			uint8_t idx = (uint8_t)TALOS_CTZ(cell);
			uint8_t p = portBits4OfIndex(idx);
			origPorts[flat] = p;
			if (!TALOS_CATEGORY_NATURAL[TALOS_BIT_TO_CATEGORY[idx]])
			{
				locked[flat] = 1;
				ports[flat] = p;
			}
			else
			{
				p &= (uint8_t)~voidOobDirsUnpacked(gridLayer, width, length, x, y);
				ports[flat] = p;
			}
		}
	}

	// make interior edges symmetric in ports[]
	for (uint32_t y = 0; y < length; y++)
	{
		for (uint32_t x = 0; x < width; x++)
		{
			uint32_t a = y * width + x;
			if (gridLayer[y][x] == Empty_Tile)
				continue;

			for (uint8_t d = 0; d < 4; d++)
			{
				if (d != DIR_IDX_E && d != DIR_IDX_S)
					continue;
				int32_t nx = (int32_t)x + TOPO_DX[d];
				int32_t ny = (int32_t)y + TOPO_DY[d];
				if (nx < 0 || ny < 0 || (uint32_t)nx >= width || (uint32_t)ny >= length)
					continue;
				uint32_t b = (uint32_t)ny * width + (uint32_t)nx;
				if (gridLayer[ny][nx] == Empty_Tile)
					continue;

				uint8_t bitA = (uint8_t)((ports[a] >> d) & 1u);
				uint8_t opp = DIR_OPPOSITE(d);
				uint8_t bitB = (uint8_t)((ports[b] >> opp) & 1u);
				if (bitA == bitB)
					continue;

				if (locked[a] && !locked[b])
					ports[b] ^= (uint8_t)(1u << opp);
				else if (locked[b] && !locked[a])
					ports[a] ^= (uint8_t)(1u << d);
				else if (!locked[a] && !locked[b])
				{
					uint8_t paFlip = ports[a] ^ (uint8_t)(1u << d);
					uint8_t pbFlip = ports[b] ^ (uint8_t)(1u << opp);
					if (maskToTile[paFlip] != 0xFF && maskToTile[ports[b]] != 0xFF)
						ports[a] = paFlip;
					else if (maskToTile[pbFlip] != 0xFF && maskToTile[ports[a]] != 0xFF)
						ports[b] = pbFlip;
					else if (maskToTile[ports[a]] == 0xFF && maskToTile[paFlip] != 0xFF)
						ports[a] = paFlip;
					else if (maskToTile[ports[b]] == 0xFF && maskToTile[pbFlip] != 0xFF)
						ports[b] = pbFlip;
					else if (popcount4(ports[a]) > 2 || (popcount4(ports[a]) == 2 && bitA))
						ports[b] = pbFlip;
					else
						ports[a] = paFlip;
				}
			}
		}
	}

	// pair remaining invalid cells with edge-flip bfs
	for (uint32_t sy = 0; sy < length; sy++)
	{
		for (uint32_t sx = 0; sx < width; sx++)
		{
			uint32_t u = sy * width + sx;
			if (locked[u] || ports[u] == 0 || maskToTile[ports[u]] != 0xFF)
				continue;

			memset(vis, 0, (size_t)totalCells * 4);
			int qHead = 0, qTail = 0;
			int32_t foundState = -1;

			for (uint8_t d = 0; d < 4; d++)
			{
				uint8_t startNew = ports[u] ^ (uint8_t)(1u << d);
				if (maskToTile[startNew] == 0xFF)
					continue;
				int32_t nx = (int32_t)sx + TOPO_DX[d];
				int32_t ny = (int32_t)sy + TOPO_DY[d];
				if (nx < 0 || ny < 0 || (uint32_t)nx >= width || (uint32_t)ny >= length)
					continue;
				uint32_t nb = (uint32_t)ny * width + (uint32_t)nx;
				if (locked[nb])
					continue;

				uint8_t opp = DIR_OPPOSITE(d);
				if (ports[nb] != 0 && maskToTile[ports[nb]] == 0xFF)
				{
					uint8_t endNew = ports[nb] ^ (uint8_t)(1u << opp);
					if (maskToTile[endNew] != 0xFF)
					{
						ports[u] = startNew;
						ports[nb] = endNew;
						foundState = -2;
						break;
					}
				}

				int32_t st = (int32_t)(nb * 4 + opp);
				if (!vis[st])
				{
					vis[st] = 1;
					prevState[st] = -1 - (int32_t)d;
					bfsQ[qTail++] = st;
				}
			}

			if (foundState == -2)
				continue;

			while (qHead < qTail && foundState < 0)
			{
				int32_t st = bfsQ[qHead++];
				uint32_t c = (uint32_t)(st / 4);
				uint8_t inD = (uint8_t)(st & 3);
				uint32_t cx = c % width;
				uint32_t cy = c / width;

				if (c != u && ports[c] != 0 && maskToTile[ports[c]] == 0xFF)
				{
					uint8_t endNew = ports[c] ^ (uint8_t)(1u << inD);
					if (maskToTile[endNew] != 0xFF)
					{
						foundState = st;
						break;
					}
				}

				for (uint8_t outD = 0; outD < 4; outD++)
				{
					if (outD == inD)
						continue;
					uint8_t midNew = ports[c] ^ (uint8_t)(1u << inD) ^ (uint8_t)(1u << outD);
					if (maskToTile[midNew] == 0xFF)
						continue;

					int32_t nx = (int32_t)cx + TOPO_DX[outD];
					int32_t ny = (int32_t)cy + TOPO_DY[outD];
					if (nx < 0 || ny < 0 || (uint32_t)nx >= width || (uint32_t)ny >= length)
						continue;
					uint32_t nb = (uint32_t)ny * width + (uint32_t)nx;
					if (locked[nb])
						continue;

					int32_t nst = (int32_t)(nb * 4 + DIR_OPPOSITE(outD));
					if (!vis[nst])
					{
						vis[nst] = 1;
						prevState[nst] = st;
						bfsQ[qTail++] = nst;
					}
				}
			}

			if (foundState >= 0)
			{
				// trace path and verify flipped tiles stay valid
				uint8_t *tmpPorts = malloc(totalCells);
				if (tmpPorts)
				{
					memcpy(tmpPorts, ports, totalCells);
					int32_t cur = foundState;
					int steps = 0;
					while (cur >= 0 && steps < (int)totalCells)
					{
						steps++;
						uint32_t c = (uint32_t)(cur / 4);
						uint8_t inD = (uint8_t)(cur & 3);
						int32_t pr = prevState[cur];
						tmpPorts[c] ^= (uint8_t)(1u << inD);

						if (pr < 0)
						{
							uint8_t outD = (uint8_t)(-pr - 1);
							tmpPorts[u] ^= (uint8_t)(1u << outD);
							break;
						}
						else
						{
							uint32_t pCell = (uint32_t)(pr / 4);
							tmpPorts[pCell] ^= (uint8_t)(1u << DIR_OPPOSITE(inD));
							cur = pr;
						}
					}

					bool okPath = (steps < (int)totalCells);
					if (okPath)
					{
						if (maskToTile[tmpPorts[u]] == 0xFF)
							okPath = false;
						int32_t vCur = foundState;
						int vSteps = 0;
						while (vCur >= 0 && okPath && vSteps < steps)
						{
							vSteps++;
							uint32_t c = (uint32_t)(vCur / 4);
							if (maskToTile[tmpPorts[c]] == 0xFF)
								okPath = false;
							vCur = prevState[vCur];
						}
					}
					if (okPath)
						memcpy(ports, tmpPorts, totalCells);
					free(tmpPorts);
				}
			}
		}
	}

	// write repaired tiles back to gridLayer
	for (uint32_t y = 0; y < length; y++)
	{
		for (uint32_t x = 0; x < width; x++)
		{
			uint32_t flat = y * width + x;
			if (locked[flat])
				continue;

			uint8_t p = ports[flat];
			if (maskToTile[p] != 0xFF)
			{
				if (p != origPorts[flat])
				{
					gridLayer[y][x] = TALOS_BIT(maskToTile[p]);
					changed = true;
				}
			}
			else if (p != origPorts[flat])
			{
				uint8_t voidDirs = voidOobDirsUnpacked(gridLayer, width, length, x, y);
				uint8_t bestT = 0xFF;
				int bestScore = -1;
				for (int t = 0; t < TALOS_NUM_TILES; t++)
				{
					if (!TALOS_CATEGORY_NATURAL[TALOS_BIT_TO_CATEGORY[t]])
						continue;
					uint8_t tp = portBits4OfIndex((uint8_t)t);
					if (tp & voidDirs)
						continue;
					int shared = (int)popcount4((uint8_t)(tp & p));
					int diff = (int)popcount4((uint8_t)(tp ^ p));
					int sc = shared * 8 - diff;
					if (sc > bestScore)
					{
						bestScore = sc;
						bestT = (uint8_t)t;
					}
				}
				talos_tile_t nextM = (bestT == 0xFF) ? Empty_Tile : TALOS_BIT(bestT);
				if (nextM != gridLayer[y][x])
				{
					gridLayer[y][x] = nextM;
					changed = true;
				}
			}
		}
	}

	free(origPorts);
	free(ports);
	free(locked);
	free(vis);
	free(prevState);
	free(bfsQ);
	return changed;
}

bool repairMazePortsInPlace(talos_tile_t **gridLayer, uint32_t width, uint32_t length)
{
	TALOS_PROFILE_FUNC();
	if (hasAnyOpenPortMask() == Empty_Tile)
		return false;

	bool changed = false;
	bool needPathFlip = false;
	bool needPass2 = false;

	// pass 1: fix simple half-open walls and flag if any void leaks or harder mismatches exist
	for (uint32_t y = 0; y < length; y++)
	{
		for (uint32_t x = 0; x < width; x++)
		{
			talos_tile_t cell = gridLayer[y][x];
			if (cell == Empty_Tile)
				continue;

			for (uint8_t d = 0; d < 4; d++)
			{
				if (!(cell & TALOS_HAS_PORT_MASK[d]))
					continue;

				int32_t nx = (int32_t)x + TOPO_DX[d];
				int32_t ny = (int32_t)y + TOPO_DY[d];
				if (nx < 0 || ny < 0 || (uint32_t)nx >= width || (uint32_t)ny >= length)
				{
					needPass2 = true;
					continue;
				}

				talos_tile_t nb = gridLayer[ny][nx];
				uint8_t opp = DIR_OPPOSITE(d);
				if (nb == Empty_Tile)
				{
					needPass2 = true;
					continue;
				}
				if (nb & TALOS_HAS_PORT_MASK[opp])
					continue;

				uint8_t idxB = (uint8_t)TALOS_CTZ(nb);
				if (!TALOS_CATEGORY_NATURAL[TALOS_BIT_TO_CATEGORY[idxB]])
				{
					needPathFlip = true;
					needPass2 = true;
					continue;
				}

				uint8_t curB = portBits4OfIndex(idxB);
				uint8_t nextB = TALOS_OPEN_LUT[idxB][opp];
				uint8_t nextPortsB = portBits4OfIndex(nextB);
				if (nextB != idxB &&
					TALOS_CATEGORY_NATURAL[TALOS_BIT_TO_CATEGORY[nextB]] &&
					(nextPortsB & (1u << opp)))
				{
					uint8_t extraB = (uint8_t)(nextPortsB & ~(curB | (1u << opp)));
					if (extraB == 0)
					{
						gridLayer[ny][nx] = TALOS_BIT(nextB);
						changed = true;
						continue;
					}
					if ((extraB & voidOobDirsUnpacked(gridLayer, width, length, (uint32_t)nx, (uint32_t)ny)) == 0)
					{
						gridLayer[ny][nx] = TALOS_BIT(nextB);
						changed = true;
						needPathFlip = true;
						needPass2 = true;
						continue;
					}
				}

				needPathFlip = true;
				needPass2 = true;
			}
		}
	}

	if (!needPass2)
		return changed;

	// edge-flip repair for sparse/even-degree tilesets
	if (needPathFlip && healMismatchesByPathFlip(gridLayer, width, length))
		changed = true;

	// pass 2: queue cleanup on remaining bad cells
	uint32_t totalCells = width * length;
	TopoNode *q = NULL;
	uint8_t *inQ = NULL;
	uint32_t head = 0, tail = 0, count = 0;

	for (uint32_t y = 0; y < length; y++)
	{
		for (uint32_t x = 0; x < width; x++)
		{
			talos_tile_t cell = gridLayer[y][x];
			if (cell == Empty_Tile)
				continue;

			bool hasBad = false;
			for (uint8_t d = 0; d < 4; d++)
			{
				if (!(cell & TALOS_HAS_PORT_MASK[d]))
					continue;
				int32_t nx = (int32_t)x + TOPO_DX[d];
				int32_t ny = (int32_t)y + TOPO_DY[d];
				if (nx < 0 || ny < 0 || (uint32_t)nx >= width || (uint32_t)ny >= length ||
					gridLayer[ny][nx] == Empty_Tile ||
					!(gridLayer[ny][nx] & TALOS_HAS_PORT_MASK[DIR_OPPOSITE(d)]))
				{
					hasBad = true;
					break;
				}
			}

			if (!hasBad)
				continue;

			if (!q)
			{
				q = malloc(sizeof(TopoNode) * totalCells);
				inQ = calloc(totalCells, sizeof(uint8_t));
				if (!q || !inQ)
				{
					free(q);
					free(inQ);
					return changed;
				}
			}

			q[tail++] = (TopoNode){ x, y };
			if (tail == totalCells)
				tail = 0;
			count++;
			inQ[y * width + x] = 1;
		}
	}

	if (!q)
		return changed;

	while (count > 0)
	{
		TopoNode c = q[head++];
		if (head == totalCells)
			head = 0;
		count--;
		inQ[c.y * width + c.x] = 0;

		talos_tile_t cell = gridLayer[c.y][c.x];
		if (cell == Empty_Tile)
			continue;

		for (uint8_t d = 0; d < 4; d++)
		{
			cell = gridLayer[c.y][c.x];
			if (cell == Empty_Tile)
				break;
			if (!(cell & TALOS_HAS_PORT_MASK[d]))
				continue;

			int32_t nx = (int32_t)c.x + TOPO_DX[d];
			int32_t ny = (int32_t)c.y + TOPO_DY[d];
			bool isBad = false;
			if (nx < 0 || ny < 0 || (uint32_t)nx >= width || (uint32_t)ny >= length)
			{
				isBad = true;
			}
			else
			{
				talos_tile_t nb = gridLayer[ny][nx];
				if (nb == Empty_Tile || !(nb & TALOS_HAS_PORT_MASK[DIR_OPPOSITE(d)]))
					isBad = true;
			}

			if (!isBad)
				continue;

			uint8_t curIdx = (uint8_t)TALOS_CTZ(cell);
			uint8_t repIdx = findSafeCloseReplacement(gridLayer, width, length, c.x, c.y, curIdx, d);
			talos_tile_t nextMask = (repIdx == 0xFF) ? Empty_Tile : TALOS_BIT(repIdx);
			if (nextMask == cell)
				continue;

			gridLayer[c.y][c.x] = nextMask;
			changed = true;

			if (nextMask != Empty_Tile && !inQ[c.y * width + c.x])
			{
				q[tail++] = c;
				if (tail == totalCells)
					tail = 0;
				count++;
				inQ[c.y * width + c.x] = 1;
			}

			for (uint8_t k = 0; k < 4; k++)
			{
				int32_t ax = (int32_t)c.x + TOPO_DX[k];
				int32_t ay = (int32_t)c.y + TOPO_DY[k];
				if (ax < 0 || ay < 0 || (uint32_t)ax >= width || (uint32_t)ay >= length)
					continue;
				talos_tile_t acell = gridLayer[ay][ax];
				uint32_t aFlat = (uint32_t)ay * width + (uint32_t)ax;
				if (acell != Empty_Tile && (acell & TALOS_HAS_PORT_MASK[DIR_OPPOSITE(k)]) && !inQ[aFlat])
				{
					q[tail++] = (TopoNode){ (uint32_t)ax, (uint32_t)ay };
					if (tail == totalCells)
						tail = 0;
					count++;
					inQ[aFlat] = 1;
				}
			}
			break;
		}
	}

	free(q);
	free(inQ);
	return changed;
}

void pruneUnreachableComponentsInPlace(talos_tile_t **gridLayer, uint32_t width, uint32_t length)
{
	TALOS_PROFILE_FUNC();
	if (hasAnyOpenPortMask() == Empty_Tile)
		return;

	uint32_t totalCells = width * length;
	int32_t *compId = malloc(sizeof(int32_t) * totalCells);
	TopoNode *q = malloc(sizeof(TopoNode) * totalCells);
	if (!compId || !q)
	{
		free(compId);
		free(q);
		return;
	}
	for (uint32_t i = 0; i < totalCells; i++)
		compId[i] = -1;

	int32_t numComps = 0;
	int32_t bestComp = -1;
	uint32_t bestSize = 0;

	for (uint32_t sy = 0; sy < length; sy++)
	{
		for (uint32_t sx = 0; sx < width; sx++)
		{
			if (gridLayer[sy][sx] == Empty_Tile || compId[sy * width + sx] != -1)
				continue;

			int32_t cid = numComps++;
			uint32_t size = 0;
			int head = 0, tail = 0;
			compId[sy * width + sx] = cid;
			q[tail++] = (TopoNode){ sx, sy };

			while (head < tail)
			{
				TopoNode c = q[head++];
				size++;
				talos_tile_t cell = gridLayer[c.y][c.x];

				for (uint8_t d = 0; d < 4; d++)
				{
					if (!(cell & TALOS_HAS_PORT_MASK[d]))
						continue;
					int32_t nx = (int32_t)c.x + TOPO_DX[d];
					int32_t ny = (int32_t)c.y + TOPO_DY[d];
					if (nx < 0 || ny < 0 || (uint32_t)nx >= width || (uint32_t)ny >= length)
						continue;

					uint32_t nFlat = (uint32_t)ny * width + (uint32_t)nx;
					if (compId[nFlat] != -1)
						continue;

					talos_tile_t nb = gridLayer[ny][nx];
					if (nb != Empty_Tile && (nb & TALOS_HAS_PORT_MASK[DIR_OPPOSITE(d)]))
					{
						compId[nFlat] = cid;
						q[tail++] = (TopoNode){ (uint32_t)nx, (uint32_t)ny };
					}
				}
			}

			if (size > bestSize)
			{
				bestSize = size;
				bestComp = cid;
			}
		}
	}

	if (numComps > 1 && bestComp != -1)
	{
		for (uint32_t y = 0; y < length; y++)
		{
			for (uint32_t x = 0; x < width; x++)
			{
				if (gridLayer[y][x] != Empty_Tile && compId[y * width + x] != bestComp)
					gridLayer[y][x] = Empty_Tile;
			}
		}
	}

	free(compId);
	free(q);
}
