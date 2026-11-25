#ifndef __TILES__
#define __TILES__

#include <stdint.h>
#include <stdbool.h>

/*
	tile definitions now come from tileset.json

	talos_codegen reads tileset.json and writes talos_tileset_baked.h, which contains:
	  - talos_tile_t        (uint8_t / uint16_t / uint32_t / uint64_t, picked from the tile count)
	  - Empty_Tile, All_Possible_State
	  - one macro per tile  (North_East_Corridor, Normal_X_Corridor, ... = TALOS_BIT(id))
	  - TALOS_HAS_PORT_MASK, TALOS_PROP_MASK, TALOS_OPEN_LUT, TALOS_CLOSE_LUT
	  - category tables     (TALOS_BIT_TO_CATEGORY, TALOS_CATEGORY_*)
	  - architect anchors   (TALOS_ANCHOR_*)

	DO NOT edit talos_tileset_baked.h by hand, edit tileset.json and re-run talos_codegen
*/
#ifdef TALOS_SOFT_MODE
#include "talos_soft.h"
#elif defined(TALOS_USE_CMAKE_BAKED)
#include <talos_tileset_baked.h>
#else
#include "talos_tileset_baked.h"
#endif

#ifndef TALOS_MAX_CATEGORIES
#define TALOS_MAX_CATEGORIES 16
#endif

#ifndef TALOS_NUM_SOCKETS
#define TALOS_NUM_SOCKETS 2
#endif

// standardized direction indices used to index every LUT (0=N, 1=E, 2=S, 3=W, 4=U, 5=D)
#ifndef DIR_IDX_N
#define DIR_IDX_N 0
#define DIR_IDX_E 1
#define DIR_IDX_S 2
#define DIR_IDX_W 3
#define DIR_IDX_U 4
#define DIR_IDX_D 5
#endif

#ifndef VERT_IDX_UP
#define VERT_IDX_UP   0
#define VERT_IDX_DOWN 1
#endif

#ifndef DIR_OPPOSITE
#define DIR_OPPOSITE(d) (((d) + 2) & 3)
#endif

// mask -> index (position of the single set bit)
static inline uint8_t maskToIndex(talos_tile_t mask)
{
	return (mask == Empty_Tile) ? 0 : (uint8_t)TALOS_CTZ(mask);
}

// index -> mask
static inline talos_tile_t indexToMask(uint8_t index)
{
	return TALOS_BIT(index);
}

#endif
