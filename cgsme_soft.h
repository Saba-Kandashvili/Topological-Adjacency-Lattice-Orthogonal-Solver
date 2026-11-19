#ifndef CGSME_SOFT_H
#define CGSME_SOFT_H

#include <stdint.h>
#include <stdbool.h>
#include "cgsme_ruleset.h"

extern CgsmeRuleset g_cgsme_soft_ruleset;

bool cgsme_soft_load_ruleset(const char *json_path);
const CgsmeRuleset *cgsme_soft_get_ruleset(void);
uint64_t ***generateGrid_Soft(uint32_t width, uint32_t length, uint32_t height, uint32_t seed, uint32_t fulness);
void freeGrid_Soft(uint64_t ***grid, uint32_t width, uint32_t length, uint32_t height);

#ifdef CGSME_SOFT_MODE

typedef uint64_t cgsme_tile_t;

#define CGSME_CONTAINER_BITS          64
#define CGSME_NUM_TILES               ((int)g_cgsme_soft_ruleset.num_tiles)
#define CGSME_NUM_SOCKETS             ((int)g_cgsme_soft_ruleset.num_horiz_sockets)
#define CGSME_NUM_CATEGORIES          ((int)g_cgsme_soft_ruleset.num_categories)
#define CGSME_NUM_ANCHORS             ((int)g_cgsme_soft_ruleset.num_anchors)
#define CGSME_GUARANTEE_LAYER_CONN    (g_cgsme_soft_ruleset.guarantee_layer_connection)
#define CGSME_TILE_INDEX_BITS         (g_cgsme_soft_ruleset.tile_index_bits)
#define CGSME_TILE_INDEX_MASK         ((cgsme_tile_t)((1ULL << g_cgsme_soft_ruleset.tile_index_bits) - 1ULL))
#define CGSME_VISITED_BIT             ((cgsme_tile_t)(1ULL << g_cgsme_soft_ruleset.tile_index_bits))
#define CGSME_PACKED_VOID             ((cgsme_tile_t)-1)

#if !defined(CGSME_FORCE_PORTABLE_BITOPS) && (defined(__GNUC__) || defined(__clang__))
#define CGSME_POPCOUNT(x)             __builtin_popcountll((uint64_t)(x))
#define CGSME_CTZ(x)                  __builtin_ctzll((uint64_t)(x))
#else
static inline int cgsme_soft_popcount64_portable(uint64_t x) {
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (int)((x * 0x0101010101010101ULL) >> 56);
}
static inline int cgsme_soft_ctz64_portable(uint64_t x) {
    static const int tbl[32] = {
        0, 1, 28, 2, 29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4, 8,
        31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6, 11, 5, 10, 9
    };
    uint32_t lo = (uint32_t)x;
    if (lo != 0)
        return tbl[(uint32_t)(((lo & (0u - lo)) * 0x077CB531u) >> 27)];
    uint32_t hi = (uint32_t)(x >> 32);
    return (hi != 0) ? (32 + tbl[(uint32_t)(((hi & (0u - hi)) * 0x077CB531u) >> 27)]) : 64;
}
#define CGSME_POPCOUNT(x)             cgsme_soft_popcount64_portable((uint64_t)(x))
#define CGSME_CTZ(x)                  cgsme_soft_ctz64_portable((uint64_t)(x))
#endif

#define CGSME_BIT(i)                  (((cgsme_tile_t)1ULL) << (i))
#define Empty_Tile                    ((cgsme_tile_t)0ULL)
#define All_Possible_State            ((cgsme_tile_t)g_cgsme_soft_ruleset.all_possible)

#define CGSME_SEED_TILE_ID            (g_cgsme_soft_ruleset.seed_tile_id)
#define CGSME_SEED_TILE_MASK          CGSME_BIT(g_cgsme_soft_ruleset.seed_tile_id)
#define CGSME_GAUSS_WIDTH             (g_cgsme_soft_ruleset.gauss_width)
#define CGSME_CONNECTOR_BOOST         (g_cgsme_soft_ruleset.connector_boost)

#define CGSME_HAS_PORT_MASK           (g_cgsme_soft_ruleset.has_port_mask)
#define CGSME_PROP_MASK               (g_cgsme_soft_ruleset.prop_mask)
#define CGSME_VERT_PROP_MASK          (g_cgsme_soft_ruleset.vert_prop_mask)
#define CGSME_OPEN_LUT                (g_cgsme_soft_ruleset.open_lut)
#define CGSME_CLOSE_LUT               (g_cgsme_soft_ruleset.close_lut)

#define CGSME_BIT_TO_CATEGORY         (g_cgsme_soft_ruleset.bit_to_category)
#define CGSME_CATEGORY_CURVE_POS      (g_cgsme_soft_ruleset.category_curve_pos)
#define CGSME_CATEGORY_MASK_WEIGHT    (g_cgsme_soft_ruleset.category_mask_weight)
#define CGSME_CATEGORY_BOOST          (g_cgsme_soft_ruleset.category_boost)
#define CGSME_CATEGORY_NATURAL        (g_cgsme_soft_ruleset.category_natural)

#define CGSME_ANCHOR_LOWER            (g_cgsme_soft_ruleset.anchor_lower)
#define CGSME_ANCHOR_UPPER            (g_cgsme_soft_ruleset.anchor_upper)
#define CGSME_ANCHOR_AREA_SIZE        (g_cgsme_soft_ruleset.anchor_area_size)
#define CGSME_ANCHOR_COUNT_PER_AREA   (g_cgsme_soft_ruleset.anchor_count_per_area)
#define CGSME_ANCHOR_DIVISOR          (g_cgsme_soft_ruleset.anchor_divisor)
#define CGSME_ANCHOR_MIN              (g_cgsme_soft_ruleset.anchor_min)
#define CGSME_ANCHOR_SPACING          (g_cgsme_soft_ruleset.anchor_spacing)
#define CGSME_ANCHOR_ALLOW_STACKING   (g_cgsme_soft_ruleset.anchor_allow_stacking)

#endif // CGSME_SOFT_MODE

#endif // CGSME_SOFT_H
