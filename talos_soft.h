#ifndef TALOS_SOFT_H
#define TALOS_SOFT_H

#include <stdint.h>
#include <stdbool.h>
#include "talos_ruleset.h"

extern TalosRuleset g_talos_soft_ruleset;

bool talos_soft_load_ruleset(const char *json_path);
const TalosRuleset *talos_soft_get_ruleset(void);
uint64_t ***generateGrid_Soft(uint32_t width, uint32_t length, uint32_t height, uint32_t seed, uint32_t fulness);
void freeGrid_Soft(uint64_t ***grid, uint32_t width, uint32_t length, uint32_t height);

#ifdef TALOS_SOFT_MODE

typedef uint64_t talos_tile_t;

#define TALOS_CONTAINER_BITS          64
#define TALOS_NUM_TILES               ((int)g_talos_soft_ruleset.num_tiles)
#define TALOS_NUM_SOCKETS             ((int)g_talos_soft_ruleset.num_horiz_sockets)
#define TALOS_NUM_CATEGORIES          ((int)g_talos_soft_ruleset.num_categories)
#define TALOS_NUM_ANCHORS             ((int)g_talos_soft_ruleset.num_anchors)
#define TALOS_GUARANTEE_LAYER_CONN    (g_talos_soft_ruleset.guarantee_layer_connection)
#define TALOS_TILE_INDEX_BITS         (g_talos_soft_ruleset.tile_index_bits)
#define TALOS_TILE_INDEX_MASK         ((talos_tile_t)((1ULL << g_talos_soft_ruleset.tile_index_bits) - 1ULL))
#define TALOS_VISITED_BIT             ((talos_tile_t)(1ULL << g_talos_soft_ruleset.tile_index_bits))
#define TALOS_PACKED_VOID             ((talos_tile_t)-1)

#if !defined(TALOS_FORCE_PORTABLE_BITOPS) && (defined(__GNUC__) || defined(__clang__))
#define TALOS_POPCOUNT(x)             __builtin_popcountll((uint64_t)(x))
#define TALOS_CTZ(x)                  __builtin_ctzll((uint64_t)(x))
#else
static inline int talos_soft_popcount64_portable(uint64_t x) {
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (int)((x * 0x0101010101010101ULL) >> 56);
}
static inline int talos_soft_ctz64_portable(uint64_t x) {
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
#define TALOS_POPCOUNT(x)             talos_soft_popcount64_portable((uint64_t)(x))
#define TALOS_CTZ(x)                  talos_soft_ctz64_portable((uint64_t)(x))
#endif

#define TALOS_BIT(i)                  (((talos_tile_t)1ULL) << (i))
#define Empty_Tile                    ((talos_tile_t)0ULL)
#define All_Possible_State            ((talos_tile_t)g_talos_soft_ruleset.all_possible)

#define TALOS_SEED_TILE_ID            (g_talos_soft_ruleset.seed_tile_id)
#define TALOS_SEED_TILE_MASK          TALOS_BIT(g_talos_soft_ruleset.seed_tile_id)
#define TALOS_GAUSS_WIDTH             (g_talos_soft_ruleset.gauss_width)
#define TALOS_CONNECTOR_BOOST         (g_talos_soft_ruleset.connector_boost)

#define TALOS_HAS_PORT_MASK           (g_talos_soft_ruleset.has_port_mask)
#define TALOS_PROP_MASK               (g_talos_soft_ruleset.prop_mask)
#define TALOS_VERT_PROP_MASK          (g_talos_soft_ruleset.vert_prop_mask)
#define TALOS_OPEN_LUT                (g_talos_soft_ruleset.open_lut)
#define TALOS_CLOSE_LUT               (g_talos_soft_ruleset.close_lut)

#define TALOS_BIT_TO_CATEGORY         (g_talos_soft_ruleset.bit_to_category)
#define TALOS_CATEGORY_CURVE_POS      (g_talos_soft_ruleset.category_curve_pos)
#define TALOS_CATEGORY_MASK_WEIGHT    (g_talos_soft_ruleset.category_mask_weight)
#define TALOS_CATEGORY_BOOST          (g_talos_soft_ruleset.category_boost)
#define TALOS_CATEGORY_NATURAL        (g_talos_soft_ruleset.category_natural)

#define TALOS_ANCHOR_LOWER            (g_talos_soft_ruleset.anchor_lower)
#define TALOS_ANCHOR_UPPER            (g_talos_soft_ruleset.anchor_upper)
#define TALOS_ANCHOR_AREA_SIZE        (g_talos_soft_ruleset.anchor_area_size)
#define TALOS_ANCHOR_COUNT_PER_AREA   (g_talos_soft_ruleset.anchor_count_per_area)
#define TALOS_ANCHOR_DIVISOR          (g_talos_soft_ruleset.anchor_divisor)
#define TALOS_ANCHOR_MIN              (g_talos_soft_ruleset.anchor_min)
#define TALOS_ANCHOR_SPACING          (g_talos_soft_ruleset.anchor_spacing)
#define TALOS_ANCHOR_ALLOW_STACKING   (g_talos_soft_ruleset.anchor_allow_stacking)

#endif // TALOS_SOFT_MODE

#endif // TALOS_SOFT_H
