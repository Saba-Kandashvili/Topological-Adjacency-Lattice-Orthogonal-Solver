#ifndef TALOS_RULESET_H
#define TALOS_RULESET_H

#include <stdint.h>
#include <stdbool.h>

#define TALOS_MAX_TILES      64
#define TALOS_MAX_SOCKETS    16
#define TALOS_MAX_CATEGORIES 16
#define TALOS_MAX_ANCHORS    8

// standardized direction indices (0=N, 1=E, 2=S, 3=W, 4=U, 5=D)
#define DIR_IDX_N 0
#define DIR_IDX_E 1
#define DIR_IDX_S 2
#define DIR_IDX_W 3
#define DIR_IDX_U 4
#define DIR_IDX_D 5

#define VERT_IDX_UP   0
#define VERT_IDX_DOWN 1

// opposite direction helper N(0)<->S(2) E(1)<->W(3)
#define DIR_OPPOSITE(d) (((d) + 2) & 3)

typedef struct {
    char name[32];
    bool is_open; // true if traversable/weldable path
    bool allow_void; // true if allowed adjacent to Empty_Tile (Void) and outer map borders
    uint16_t compatible_mask; // bitmask of socket IDs this socket can connect to
} TalosSocketDef;

typedef struct {
    char name[32];
    float curve_pos;
    float mask_weight;
    bool connector_boost;
    bool natural_spawn;
} TalosCategoryDef;

typedef struct {
    char name[64];
    uint8_t id;
    uint8_t category_idx;
    uint8_t ports[6]; // socket index for N(0), E(1), S(2), W(3), U(4), D(5)
    bool has_vert_port[2]; // true if U(0) or D(1) socket was explicitly set or defaulted
    bool weld_target; // welder/edge-fixer morph another tile into this one
} TalosTileDef;

typedef struct {
    uint8_t lower_tile_id;
    uint8_t upper_tile_id; // 0xFF if resolved purely via U/D sockets
    uint32_t area_size;
    uint32_t count_per_area;
    uint32_t density_divisor;
    uint32_t min_per_layer;
    uint32_t min_spacing;
    bool allow_stacking;
} TalosVerticalAnchor;

typedef struct {
    char name[64];
    uint8_t num_tiles;
    uint8_t num_sockets;
    uint8_t num_horiz_sockets;
    uint8_t num_categories;
    uint8_t num_anchors;

    // dynamically computed bit width info
    uint8_t container_bits;  // 8, 16, 32, or 64
    uint8_t tile_index_bits; // ceil(log2(num_tiles))  4 for 16 tiles
    uint64_t all_possible;   // (1ULL << num_tiles) - 1

    float gauss_width;
    float connector_boost;
    uint8_t seed_tile_id;
    bool guarantee_layer_connection;

    TalosSocketDef sockets[TALOS_MAX_SOCKETS];
    TalosCategoryDef categories[TALOS_MAX_CATEGORIES];
    TalosTileDef tiles[TALOS_MAX_TILES];
    TalosVerticalAnchor anchors[TALOS_MAX_ANCHORS];

    // PRECOMPUTED LOOKUP TABLES (LUTs)
    // self opening mask per direction: bit t is 1 if tile t has an open port on side d
    uint64_t has_port_mask[4];

    // neighbor propagation mask: for tile t (0..num_tiles-1) or empty (index == num_tiles)
    //    prop_mask[t][d] is the bitmask of valid tiles for the neighbor in direction d (N,E,S,W)
    uint64_t prop_mask[TALOS_MAX_TILES + 1][4];

    // vertical neighbor masks (0 = z+1 above, 1 = z-1 below, slot num_tiles = vertical void)
    uint64_t vert_prop_mask[TALOS_MAX_TILES + 1][2];

    // topology mutation LUTs for german/ariadne Welder and edge fixup
    //    open_lut[t][d]  = new tile ID when opening direction d on tile t
    //    close_lut[t][d] = new tile ID when closing direction d on tile t (or TALOS_EMPTY_INDEX if all closed)
    uint8_t open_lut[TALOS_MAX_TILES][4];
    uint8_t close_lut[TALOS_MAX_TILES][4];

    // flat category & architect anchor tables (used directly by --soft runtime mode and codegen)
    uint8_t  bit_to_category[TALOS_MAX_TILES];
    float    category_curve_pos[TALOS_MAX_CATEGORIES];
    float    category_mask_weight[TALOS_MAX_CATEGORIES];
    bool     category_boost[TALOS_MAX_CATEGORIES];
    bool     category_natural[TALOS_MAX_CATEGORIES];
    uint8_t  anchor_lower[TALOS_MAX_ANCHORS];
    uint8_t  anchor_upper[TALOS_MAX_ANCHORS];
    uint32_t anchor_area_size[TALOS_MAX_ANCHORS];
    uint32_t anchor_count_per_area[TALOS_MAX_ANCHORS];
    uint32_t anchor_divisor[TALOS_MAX_ANCHORS];
    uint32_t anchor_min[TALOS_MAX_ANCHORS];
    uint32_t anchor_spacing[TALOS_MAX_ANCHORS];
    bool     anchor_allow_stacking[TALOS_MAX_ANCHORS];
} TalosRuleset;

bool talos_ruleset_load_from_file(const char *filepath, TalosRuleset *out_rules);
void talos_ruleset_build_tables(TalosRuleset *rules);

#endif