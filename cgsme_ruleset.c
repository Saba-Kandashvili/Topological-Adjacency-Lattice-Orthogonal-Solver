#include "cgsme_ruleset.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// helper 
// find socket index by name (returns -1 if not found)
static int find_socket_idx(const CgsmeRuleset *r, const char *name)
{
    for (int i = 0; i < r->num_sockets; i++)
    {
        if (strcmp(r->sockets[i].name, name) == 0)
            return i;
    }
    return -1;
}

// helper
// find category index by name (returns -1 if not found)
static int find_category_idx(const CgsmeRuleset *r, const char *name)
{
    for (int i = 0; i < r->num_categories; i++)
    {
        if (strcmp(r->categories[i].name, name) == 0)
            return i;
    }
    return -1;
}

// helper
// find tile index by name (returns -1 if not found)
static int find_tile_idx(const CgsmeRuleset *r, const char *name)
{
    for (int i = 0; i < r->num_tiles; i++)
    {
        if (strcmp(r->tiles[i].name, name) == 0)
            return i;
    }
    return -1;
}

void cgsme_ruleset_build_tables(CgsmeRuleset *r)
{
    // choose smallest unsigned integer container (8, 16, 32, or 64 bits)
    if (r->num_tiles <= 8)       r->container_bits = 8;
    else if (r->num_tiles <= 16) r->container_bits = 16;
    else if (r->num_tiles <= 32) r->container_bits = 32;
    else                         r->container_bits = 64;

    r->all_possible = (r->num_tiles == 64) ? ~0ULL : ((1ULL << r->num_tiles) - 1ULL);

    // compute ceil(log2(num_tiles)) for compressed tile indices
    uint8_t bits = 1;
    while ((1U << bits) < r->num_tiles)
        bits++;
    r->tile_index_bits = bits;

    // count distinct horizontal (N,E,S,W) sockets
    uint16_t horiz_sock_mask = 0;
    for (int t = 0; t < r->num_tiles; t++)
    {
        for (int d = 0; d < 4; d++)
            horiz_sock_mask |= (uint16_t)(1U << r->tiles[t].ports[d]);
    }
    uint8_t h_socks = 0;
    for (int s = 0; s < r->num_sockets; s++)
    {
        if (horiz_sock_mask & (1U << s))
            h_socks++;
    }
    r->num_horiz_sockets = (h_socks > 0) ? h_socks : r->num_sockets;

    // build has_port_mask[4] bit t is 1 if tile t has an open socket on direction d
    for (int d = 0; d < 4; d++)
    {
        r->has_port_mask[d] = 0;
        for (int t = 0; t < r->num_tiles; t++)
        {
            uint8_t sock = r->tiles[t].ports[d];
            if (r->sockets[sock].is_open)
                r->has_port_mask[d] |= (1ULL << t);
        }
    }

    // build prop_mask[N + 1][4] valid neighbor tiles in direction d
    for (int t = 0; t < r->num_tiles; t++)
    {
        for (int d = 0; d < 4; d++)
        {
            uint8_t my_sock = r->tiles[t].ports[d];
            uint8_t opp_d = DIR_OPPOSITE(d);
            uint64_t valid_neighbors = 0;

            for (int t2 = 0; t2 < r->num_tiles; t2++)
            {
                uint8_t neighbor_sock = r->tiles[t2].ports[opp_d];
                if (r->sockets[my_sock].compatible_mask & (1U << neighbor_sock))
                    valid_neighbors |= (1ULL << t2);
            }
            r->prop_mask[t][d] = valid_neighbors;
        }
    }

    // slot num_tiles = void/map border, valid if opposite port allows void
    for (int d = 0; d < 4; d++)
    {
        uint8_t opp_d = DIR_OPPOSITE(d);
        uint64_t valid_against_void = 0;
        for (int t = 0; t < r->num_tiles; t++)
        {
            uint8_t sock = r->tiles[t].ports[opp_d];
            if (r->sockets[sock].allow_void)
                valid_against_void |= (1ULL << t);
        }
        r->prop_mask[r->num_tiles][d] = valid_against_void;
    }

    // build vert_prop_mask[N + 1][2] (0 = UP to z+1, 1 = DOWN to z-1)
    for (int t = 0; t < r->num_tiles; t++)
    {
        if (r->tiles[t].has_vert_port[VERT_IDX_UP])
        {
            uint8_t my_u = r->tiles[t].ports[DIR_IDX_U];
            uint64_t valid_up = 0;
            for (int t2 = 0; t2 < r->num_tiles; t2++)
            {
                if (!r->tiles[t2].has_vert_port[VERT_IDX_DOWN])
                {
                    valid_up |= (1ULL << t2);
                }
                else
                {
                    uint8_t nb_d = r->tiles[t2].ports[DIR_IDX_D];
                    if (r->sockets[my_u].compatible_mask & (1U << nb_d))
                        valid_up |= (1ULL << t2);
                }
            }
            r->vert_prop_mask[t][VERT_IDX_UP] = valid_up;
        }
        else
        {
            r->vert_prop_mask[t][VERT_IDX_UP] = r->all_possible;
        }

        if (r->tiles[t].has_vert_port[VERT_IDX_DOWN])
        {
            uint8_t my_d = r->tiles[t].ports[DIR_IDX_D];
            uint64_t valid_down = 0;
            for (int t2 = 0; t2 < r->num_tiles; t2++)
            {
                if (!r->tiles[t2].has_vert_port[VERT_IDX_UP])
                {
                    valid_down |= (1ULL << t2);
                }
                else
                {
                    uint8_t nb_u = r->tiles[t2].ports[DIR_IDX_U];
                    if (r->sockets[my_d].compatible_mask & (1U << nb_u))
                        valid_down |= (1ULL << t2);
                }
            }
            r->vert_prop_mask[t][VERT_IDX_DOWN] = valid_down;
        }
        else
        {
            r->vert_prop_mask[t][VERT_IDX_DOWN] = r->all_possible;
        }
    }

    // slot num_tiles for vertical void: UP checks D port for bottom floor, DOWN checks U port for top ceiling
    uint64_t valid_bottom_floor = 0;
    uint64_t valid_top_ceiling  = 0;
    for (int t = 0; t < r->num_tiles; t++)
    {
        if (!r->tiles[t].has_vert_port[VERT_IDX_DOWN] || r->sockets[r->tiles[t].ports[DIR_IDX_D]].allow_void)
            valid_bottom_floor |= (1ULL << t);
        if (!r->tiles[t].has_vert_port[VERT_IDX_UP] || r->sockets[r->tiles[t].ports[DIR_IDX_U]].allow_void)
            valid_top_ceiling |= (1ULL << t);
    }
    r->vert_prop_mask[r->num_tiles][VERT_IDX_UP]   = valid_bottom_floor;
    r->vert_prop_mask[r->num_tiles][VERT_IDX_DOWN] = valid_top_ceiling;

    // build open_lut[N][4] and close_lut[N][4] for welding and edge fixup
    for (int t = 0; t < r->num_tiles; t++)
    {
        for (int d = 0; d < 4; d++)
        {
            r->open_lut[t][d]  = (uint8_t)t; // fallback: keep current tile
            r->close_lut[t][d] = 0xFF;       // 0xFF = becomes Empty_Tile if all ports closed

            for (int mode = 0; mode < 2; mode++) // mode 0 = close_lut, mode 1 = open_lut
            {
                bool target_open[4];
                for (int k = 0; k < 4; k++)
                    target_open[k] = r->sockets[r->tiles[t].ports[k]].is_open;
                target_open[d] = (mode == 1);

                int best_tile = -1;
                int best_exact = -1;

                for (int t2 = 0; t2 < r->num_tiles; t2++)
                {
                    if (!r->tiles[t2].weld_target)
                        continue;

                    bool match = true;
                    int exact = 0;
                    for (int k = 0; k < 4; k++)
                    {
                        if (r->sockets[r->tiles[t2].ports[k]].is_open != target_open[k])
                        {
                            match = false;
                            break;
                        }
                        if (k != d && r->tiles[t2].ports[k] == r->tiles[t].ports[k])
                            exact++;
                    }
                    if (match && exact > best_exact)
                    {
                        best_exact = exact;
                        best_tile = t2;
                    }
                }

                if (mode == 1 && best_tile != -1) r->open_lut[t][d]  = (uint8_t)best_tile;
                if (mode == 0 && best_tile != -1) r->close_lut[t][d] = (uint8_t)best_tile;
            }
        }
    }

    // build flat category and anchor lookup arrays for runtime --soft mode
    for (int t = 0; t < CGSME_MAX_TILES; t++)
        r->bit_to_category[t] = (t < r->num_tiles) ? r->tiles[t].category_idx : 0u;

    for (int c = 0; c < CGSME_MAX_CATEGORIES; c++)
    {
        r->category_curve_pos[c]   = (c < r->num_categories) ? r->categories[c].curve_pos : 0.0f;
        r->category_mask_weight[c] = (c < r->num_categories) ? r->categories[c].mask_weight : 0.0f;
        r->category_boost[c]       = (c < r->num_categories) ? r->categories[c].connector_boost : false;
        r->category_natural[c]     = (c < r->num_categories) ? r->categories[c].natural_spawn : false;
    }

    for (int a = 0; a < CGSME_MAX_ANCHORS; a++)
    {
        if (a < r->num_anchors)
        {
            uint8_t lo = r->anchors[a].lower_tile_id;
            uint8_t up = r->anchors[a].upper_tile_id;

            // legacy "upper" on non-natural connector tile
            if (up != 0xFF && !r->tiles[lo].has_vert_port[VERT_IDX_UP] &&
                !r->categories[r->tiles[lo].category_idx].natural_spawn)
            {
                r->vert_prop_mask[lo][VERT_IDX_UP] = (1ULL << up);
            }

            // block stacking when allow_stacking is false
            if (!r->anchors[a].allow_stacking &&
                (r->tiles[lo].has_vert_port[VERT_IDX_UP] || !r->categories[r->tiles[lo].category_idx].natural_spawn))
            {
                r->vert_prop_mask[lo][VERT_IDX_UP]   &= ~(1ULL << lo);
                r->vert_prop_mask[lo][VERT_IDX_DOWN] &= ~(1ULL << lo);
            }

            // if upper_tile_id wasnt set, pick first valid bit from U mask
            if (up == 0xFF)
            {
                uint64_t um = r->vert_prop_mask[lo][VERT_IDX_UP];
                if (um != 0)
                {
                    uint8_t first_bit = 0;
                    while (((um >> first_bit) & 1ULL) == 0 && first_bit < r->num_tiles)
                        first_bit++;
                    up = first_bit;
                    r->anchors[a].upper_tile_id = up;
                }
                else
                {
                    up = lo;
                    r->anchors[a].upper_tile_id = up;
                }
            }

            r->anchor_lower[a]          = lo;
            r->anchor_upper[a]          = up;
            r->anchor_area_size[a]      = (r->anchors[a].area_size > 0) ? r->anchors[a].area_size : 20u;
            r->anchor_count_per_area[a] = (r->anchors[a].count_per_area > 0) ? r->anchors[a].count_per_area : 1u;
            r->anchor_divisor[a]        = (r->anchors[a].density_divisor > 0) ? r->anchors[a].density_divisor : 400u;
            r->anchor_min[a]            = r->anchors[a].min_per_layer;
            r->anchor_spacing[a]        = r->anchors[a].min_spacing;
            r->anchor_allow_stacking[a] = r->anchors[a].allow_stacking;
        }
        else
        {
            r->anchor_lower[a]          = 0u;
            r->anchor_upper[a]          = 0u;
            r->anchor_area_size[a]      = 20u;
            r->anchor_count_per_area[a] = 0u;
            r->anchor_divisor[a]        = 400u;
            r->anchor_min[a]            = 0u;
            r->anchor_spacing[a]        = 2u;
            r->anchor_allow_stacking[a] = false;
        }
    }
}

// minimal JSON string helpers
static const char *skip_ws(const char *p)
{
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',' || *p == ':'))
        p++;
    return p;
}

static const char *parse_str(const char *p, char *out, size_t max_len)
{
    p = skip_ws(p);
    if (*p != '"') return NULL;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < max_len)
        out[i++] = *p++;
    out[i] = '\0';
    if (*p == '"') p++;
    return p;
}

static const char *find_key_in_range(const char *start, const char *end, const char *key)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    size_t pat_len = strlen(pattern);
    const char *p = start;
    while (p < end)
    {
        const char *hit = strstr(p, pattern);
        if (!hit || hit >= end) return NULL;
        const char *after = skip_ws(hit + pat_len);
        return after;
    }
    return NULL;
}

static const char *find_matching_bracket(const char *open_pos, char open_ch, char close_ch)
{
    int depth = 0;
    bool in_str = false;
    for (const char *p = open_pos; *p; p++)
    {
        if (*p == '"' && (p == open_pos || *(p - 1) != '\\'))
            in_str = !in_str;
        else if (!in_str)
        {
            if (*p == open_ch) depth++;
            else if (*p == close_ch)
            {
                depth--;
                if (depth == 0) return p;
            }
        }
    }
    return NULL;
}

bool cgsme_ruleset_load_from_file(const char *filepath, CgsmeRuleset *r)
{
    FILE *f = fopen(filepath, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return false; }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len)
    {
        free(buf);
        fclose(f);
        return false;
    }
    buf[len] = '\0';
    fclose(f);

    memset(r, 0, sizeof(CgsmeRuleset));
    const char *end = buf + len;

    // toplevel name & gaussian_config
    const char *p = find_key_in_range(buf, end, "name");
    if (p) parse_str(p, r->name, sizeof(r->name));

    p = find_key_in_range(buf, end, "gauss_width");
    r->gauss_width = p ? strtof(p, NULL) : 2.0f;

    p = find_key_in_range(buf, end, "connector_boost");
    // "connector_boost" appears in categories too (search inside "gaussian_config")
    const char *gcfg = find_key_in_range(buf, end, "gaussian_config");
    if (gcfg && *gcfg == '{')
    {
        const char *gend = find_matching_bracket(gcfg, '{', '}');
        const char *cb = find_key_in_range(gcfg, gend, "connector_boost");
        r->connector_boost = cb ? strtof(cb, NULL) : 2.5f;
    }

    // parse sockets: "sockets": { "WALL": { ... }, "OPEN": { ... } }
    const char *socks_start = find_key_in_range(buf, end, "sockets");
    const char *socks_end = (socks_start && *socks_start == '{') ? find_matching_bracket(socks_start, '{', '}') : NULL;
    if (socks_start && socks_end)
    {
        // pass 1: collect socket names and "open" bool
        const char *cur = socks_start + 1;
        while (cur < socks_end && r->num_sockets < CGSME_MAX_SOCKETS)
        {
            cur = skip_ws(cur);
            if (*cur != '"') break;
            int idx = r->num_sockets++;
            cur = parse_str(cur, r->sockets[idx].name, sizeof(r->sockets[idx].name));
            cur = skip_ws(cur);
            if (*cur == '{')
            {
                const char *obj_end = find_matching_bracket(cur, '{', '}');
                const char *op = find_key_in_range(cur, obj_end, "open");
                r->sockets[idx].is_open = (op && strncmp(op, "true", 4) == 0);
                // default allow_void to !is_open unless overridden
                r->sockets[idx].allow_void = !r->sockets[idx].is_open;
                const char *av = find_key_in_range(cur, obj_end, "allow_void");
                if (av)
                    r->sockets[idx].allow_void = (strncmp(av, "true", 4) == 0);
                cur = obj_end + 1;
            }
        }
        // pass 2: resolve "compatible" socket name arrays into compatible_mask (and "VOID" token)
        cur = socks_start + 1;
        for (int idx = 0; idx < r->num_sockets; idx++)
        {
            cur = strchr(cur, '{');
            const char *obj_end = find_matching_bracket(cur, '{', '}');
            const char *comp = find_key_in_range(cur, obj_end, "compatible");
            if (comp && *comp == '[')
            {
                const char *arr_end = find_matching_bracket(comp, '[', ']');
                const char *ap = comp + 1;
                while (ap < arr_end)
                {
                    ap = skip_ws(ap);
                    if (*ap != '"') break;
                    char target_name[32];
                    ap = parse_str(ap, target_name, sizeof(target_name));
                    if (strcmp(target_name, "VOID") == 0)
                    {
                        r->sockets[idx].allow_void = true;
                    }
                    else
                    {
                        int target_idx = find_socket_idx(r, target_name);
                        if (target_idx >= 0)
                            r->sockets[idx].compatible_mask |= (1U << target_idx);
                    }
                }
            }
            cur = obj_end + 1;
        }
    }

    //  parse categories: "categories": [ { ... }, ... ]
    const char *cats_start = find_key_in_range(buf, end, "categories");
    const char *cats_end = (cats_start && *cats_start == '[') ? find_matching_bracket(cats_start, '[', ']') : NULL;
    if (cats_start && cats_end)
    {
        const char *cur = cats_start + 1;
        while (cur < cats_end && r->num_categories < CGSME_MAX_CATEGORIES)
        {
            const char *obj = strchr(cur, '{');
            if (!obj || obj >= cats_end) break;
            const char *obj_end = find_matching_bracket(obj, '{', '}');
            int idx = r->num_categories++;

            const char *v = find_key_in_range(obj, obj_end, "name");
            if (v) parse_str(v, r->categories[idx].name, sizeof(r->categories[idx].name));
            v = find_key_in_range(obj, obj_end, "curve_pos");
            if (v) r->categories[idx].curve_pos = strtof(v, NULL);
            v = find_key_in_range(obj, obj_end, "mask_weight");
            if (v) r->categories[idx].mask_weight = strtof(v, NULL);
            v = find_key_in_range(obj, obj_end, "connector_boost");
            r->categories[idx].connector_boost = (v && strncmp(v, "true", 4) == 0);
            v = find_key_in_range(obj, obj_end, "natural_spawn");
            r->categories[idx].natural_spawn = (!v || strncmp(v, "true", 4) == 0);

            cur = obj_end + 1;
        }
    }

    // parse tiles: "tiles": [ { ... }, ... ]
    const char *tiles_start = find_key_in_range(buf, end, "tiles");
    const char *tiles_end = (tiles_start && *tiles_start == '[') ? find_matching_bracket(tiles_start, '[', ']') : NULL;
    if (tiles_start && tiles_end)
    {
        const char *cur = tiles_start + 1;
        while (cur < tiles_end && r->num_tiles < CGSME_MAX_TILES)
        {
            const char *obj = strchr(cur, '{');
            if (!obj || obj >= tiles_end) break;
            const char *obj_end = find_matching_bracket(obj, '{', '}');
            int idx = r->num_tiles++;

            r->tiles[idx].id = (uint8_t)idx;
            r->tiles[idx].weld_target = true; // Default true unless "weld_target": false

            const char *v = find_key_in_range(obj, obj_end, "name");
            if (v) parse_str(v, r->tiles[idx].name, sizeof(r->tiles[idx].name));

            v = find_key_in_range(obj, obj_end, "category");
            if (v)
            {
                char cname[32];
                parse_str(v, cname, sizeof(cname));
                int cidx = find_category_idx(r, cname);
                r->tiles[idx].category_idx = (cidx >= 0) ? (uint8_t)cidx : 0;
            }

            v = find_key_in_range(obj, obj_end, "weld_target");
            if (v && strncmp(v, "false", 5) == 0)
                r->tiles[idx].weld_target = false;

            const char *ports_obj = find_key_in_range(obj, obj_end, "ports");
            if (ports_obj && *ports_obj == '{')
            {
                const char *ports_end = find_matching_bracket(ports_obj, '{', '}');
                const char *dirs[6] = {"N", "E", "S", "W", "U", "D"};
                for (int d = 0; d < 6; d++)
                {
                    const char *pv = find_key_in_range(ports_obj, ports_end, dirs[d]);
                    if (pv)
                    {
                        char sname[32];
                        parse_str(pv, sname, sizeof(sname));
                        int sidx = find_socket_idx(r, sname);
                        if (sidx >= 0)
                        {
                            r->tiles[idx].ports[d] = (uint8_t)sidx;
                            if (d == DIR_IDX_U) r->tiles[idx].has_vert_port[VERT_IDX_UP] = true;
                            if (d == DIR_IDX_D) r->tiles[idx].has_vert_port[VERT_IDX_DOWN] = true;
                        }
                    }
                }
            }
            cur = obj_end + 1;
        }
    }

    r->guarantee_layer_connection = true;

    // parse vertical_rules (or legacy architect) seed, default vertical sockets, and area_quotas / vertical_anchors
    const char *arch_start = find_key_in_range(buf, end, "vertical_rules");
    if (!arch_start)
        arch_start = find_key_in_range(buf, end, "architect");
    const char *arch_end = (arch_start && *arch_start == '{') ? find_matching_bracket(arch_start, '{', '}') : NULL;
    if (arch_start && arch_end)
    {
        const char *st = find_key_in_range(arch_start, arch_end, "seed_tile");
        if (st)
        {
            char sname[64];
            parse_str(st, sname, sizeof(sname));
            int tidx = find_tile_idx(r, sname);
            r->seed_tile_id = (tidx >= 0) ? (uint8_t)tidx : 0;
        }

        const char *glc = find_key_in_range(arch_start, arch_end, "guarantee_layer_connection");
        if (glc)
            r->guarantee_layer_connection = (strncmp(glc, "true", 4) == 0);

        const char *def_u = find_key_in_range(arch_start, arch_end, "default_up_socket");
        if (def_u)
        {
            char sname[32];
            parse_str(def_u, sname, sizeof(sname));
            int sidx = find_socket_idx(r, sname);
            if (sidx >= 0)
            {
                for (int t = 0; t < r->num_tiles; t++)
                {
                    if (!r->tiles[t].has_vert_port[VERT_IDX_UP])
                    {
                        r->tiles[t].ports[DIR_IDX_U] = (uint8_t)sidx;
                        r->tiles[t].has_vert_port[VERT_IDX_UP] = true;
                    }
                }
            }
        }

        const char *def_d = find_key_in_range(arch_start, arch_end, "default_down_socket");
        if (def_d)
        {
            char sname[32];
            parse_str(def_d, sname, sizeof(sname));
            int sidx = find_socket_idx(r, sname);
            if (sidx >= 0)
            {
                for (int t = 0; t < r->num_tiles; t++)
                {
                    if (!r->tiles[t].has_vert_port[VERT_IDX_DOWN])
                    {
                        r->tiles[t].ports[DIR_IDX_D] = (uint8_t)sidx;
                        r->tiles[t].has_vert_port[VERT_IDX_DOWN] = true;
                    }
                }
            }
        }

        const char *va = find_key_in_range(arch_start, arch_end, "area_quotas");
        if (!va) va = find_key_in_range(arch_start, arch_end, "connectors");
        if (!va) va = find_key_in_range(arch_start, arch_end, "vertical_anchors");
        const char *va_end = (va && *va == '[') ? find_matching_bracket(va, '[', ']') : NULL;
        if (va && va_end)
        {
            const char *cur = va + 1;
            while (cur < va_end && r->num_anchors < CGSME_MAX_ANCHORS)
            {
                const char *obj = strchr(cur, '{');
                if (!obj || obj >= va_end) break;
                const char *obj_end = find_matching_bracket(obj, '{', '}');
                int aidx = r->num_anchors++;

                r->anchors[aidx].lower_tile_id = 0;
                r->anchors[aidx].upper_tile_id = 0xFF;

                char tname[64];
                const char *v = find_key_in_range(obj, obj_end, "tile");
                if (!v) v = find_key_in_range(obj, obj_end, "lower");
                if (v)
                {
                    parse_str(v, tname, sizeof(tname));
                    int ti = find_tile_idx(r, tname);
                    r->anchors[aidx].lower_tile_id = (ti >= 0) ? (uint8_t)ti : 0;
                }

                v = find_key_in_range(obj, obj_end, "upper");
                if (v)
                {
                    parse_str(v, tname, sizeof(tname));
                    int ti = find_tile_idx(r, tname);
                    if (ti >= 0)
                        r->anchors[aidx].upper_tile_id = (uint8_t)ti;
                }

                v = find_key_in_range(obj, obj_end, "density_divisor");
                uint32_t div = v ? (uint32_t)strtoul(v, NULL, 10) : 400u;
                r->anchors[aidx].density_divisor = (div > 0) ? div : 400u;

                v = find_key_in_range(obj, obj_end, "area_size");
                if (!v) v = find_key_in_range(obj, obj_end, "sector_size");
                if (v)
                {
                    r->anchors[aidx].area_size = (uint32_t)strtoul(v, NULL, 10);
                }
                else
                {
                    uint32_t s = 1;
                    while ((s + 1) * (s + 1) <= r->anchors[aidx].density_divisor)
                        s++;
                    r->anchors[aidx].area_size = (s >= 4) ? s : 20u;
                }

                v = find_key_in_range(obj, obj_end, "count_per_area");
                if (!v) v = find_key_in_range(obj, obj_end, "count_per_sector");
                r->anchors[aidx].count_per_area = v ? (uint32_t)strtoul(v, NULL, 10) : 1u;

                v = find_key_in_range(obj, obj_end, "min_per_layer");
                r->anchors[aidx].min_per_layer = v ? (uint32_t)strtoul(v, NULL, 10) : 2u;

                v = find_key_in_range(obj, obj_end, "min_spacing");
                r->anchors[aidx].min_spacing = v ? (uint32_t)strtoul(v, NULL, 10) : 2u;

                v = find_key_in_range(obj, obj_end, "allow_stacking");
                r->anchors[aidx].allow_stacking = (v && strncmp(v, "true", 4) == 0);

                cur = obj_end + 1;
            }
        }
    }

    free(buf);
    cgsme_ruleset_build_tables(r);
    return (r->num_tiles > 0);
}