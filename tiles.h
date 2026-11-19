#ifndef __TILES__
#define __TILES__

#include <stdint.h>
#include <stdbool.h>

/*
	tile definitions now come from tileset.json

	cgsme_codegen reads tileset.json and writes cgsme_tileset_baked.h, which contains:
	  - cgsme_tile_t        (uint8_t / uint16_t / uint32_t / uint64_t, picked from the tile count)
	  - Empty_Tile, All_Possible_State
	  - one macro per tile  (North_East_Corridor, Normal_X_Corridor, ... = CGSME_BIT(id))
	  - CGSME_HAS_PORT_MASK, CGSME_PROP_MASK, CGSME_OPEN_LUT, CGSME_CLOSE_LUT
	  - category tables     (CGSME_BIT_TO_CATEGORY, CGSME_CATEGORY_*)
	  - architect anchors   (CGSME_ANCHOR_*)

	DO NOT edit cgsme_tileset_baked.h by hand, edit tileset.json and re-run cgsme_codegen
*/
#ifdef CGSME_SOFT_MODE
#include "cgsme_soft.h"
#elif defined(CGSME_USE_CMAKE_BAKED)
#include <cgsme_tileset_baked.h>
#else
#include "cgsme_tileset_baked.h"
#endif

#ifndef CGSME_MAX_CATEGORIES
#define CGSME_MAX_CATEGORIES 16
#endif

#ifndef CGSME_NUM_SOCKETS
#define CGSME_NUM_SOCKETS 2
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
static inline uint8_t maskToIndex(cgsme_tile_t mask)
{
	return (mask == Empty_Tile) ? 0 : (uint8_t)CGSME_CTZ(mask);
}

// index -> mask
static inline cgsme_tile_t indexToMask(uint8_t index)
{
	return CGSME_BIT(index);
}

#endif
