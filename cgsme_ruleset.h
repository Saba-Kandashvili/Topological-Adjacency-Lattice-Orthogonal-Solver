#ifndef CGSME_RULESET_H
#define CGSME_RULESET_H

#include <stdint.h>
#include <stdbool.h>

#define CGSME_MAX_TILES      64
#define CGSME_MAX_SOCKETS    16
#define CGSME_MAX_CATEGORIES 16
#define CGSME_MAX_ANCHORS    8

// standardized direction indices (0=N, 1=E, 2=S, 3=W)
#define DIR_IDX_N 0
#define DIR_IDX_E 1
#define DIR_IDX_S 2
#define DIR_IDX_W 3

// opposite direction helper N(0)<->S(2) E(1)<->W(3)
#define DIR_OPPOSITE(d) (((d) + 2) & 3)

typedef struct {
    char name[32];
    bool is_open; // true if traversable false if wall/closed
    uint16_t compatible_mask; // bitmask of socket IDs this socket can connect to
} CgsmeSocketDef;

typedef struct {
    char name[32];
    float curve_pos;
    float mask_weight;
    bool connector_boost;
    bool natural_spawn;
} CgsmeCategoryDef;

typedef struct {
    char name[64];
    uint8_t id;
    uint8_t category_idx;
    uint8_t ports[4]; // socket index for N, E, S, W
    bool weld_target; // welder/edge-fixer morph another tile into this one
} CgsmeTileDef;

typedef struct {
    uint8_t lower_tile_id;
    uint8_t upper_tile_id;
    uint32_t density_divisor;
    uint32_t min_per_layer;
} CgsmeVerticalAnchor;

typedef struct {
    char name[64];
    uint8_t num_tiles;
    uint8_t num_sockets;
    uint8_t num_categories;
    uint8_t num_anchors;

    // dynamically computed bit width info
    uint8_t container_bits;  // 8, 16, 32, or 64
    uint8_t tile_index_bits; // ceil(log2(num_tiles))  4 for 16 tiles
    uint64_t all_possible;   // (1ULL << num_tiles) - 1

    float gauss_width;
    float connector_boost;
    uint8_t seed_tile_id;

    CgsmeSocketDef sockets[CGSME_MAX_SOCKETS];
    CgsmeCategoryDef categories[CGSME_MAX_CATEGORIES];
    CgsmeTileDef tiles[CGSME_MAX_TILES];
    CgsmeVerticalAnchor anchors[CGSME_MAX_ANCHORS];

    // PRECOMPUTED LOOKUP TABLES (LUTs)
    // self opening mask per direction: bit t is 1 if tile t has an open port on side d
    uint64_t has_port_mask[4];

    // neighbor propagation mask: for tile t (0..num_tiles-1) or empty (index == num_tiles)
    //    prop_mask[t][d] is the bitmask of valid tiles for the neighbor in direction d
    uint64_t prop_mask[CGSME_MAX_TILES + 1][4];

    // topology mutation LUTs for german/ariadne Welder and edge fixup
    //    open_lut[t][d]  = new tile ID when opening direction d on tile t
    //    close_lut[t][d] = new tile ID when closing direction d on tile t (or CGSME_EMPTY_INDEX if all closed)
    uint8_t open_lut[CGSME_MAX_TILES][4];
    uint8_t close_lut[CGSME_MAX_TILES][4];
} CgsmeRuleset;

bool cgsme_ruleset_load_from_file(const char *filepath, CgsmeRuleset *out_rules);
void cgsme_ruleset_build_tables(CgsmeRuleset *rules);

#endif