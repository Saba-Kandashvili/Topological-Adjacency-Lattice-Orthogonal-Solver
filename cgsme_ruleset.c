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

    // slot num_tiles represents Empty_Tile (Void)
    // neighbors must be closed toward Void
    for (int d = 0; d < 4; d++)
    {
        uint8_t opp_d = DIR_OPPOSITE(d);
        r->prop_mask[r->num_tiles][d] = (~r->has_port_mask[opp_d]) & r->all_possible;
    }

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
                cur = obj_end + 1;
            }
        }
        // pass 2: resolve "compatible" socket name arrays into compatible_mask
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
                    int target_idx = find_socket_idx(r, target_name);
                    if (target_idx >= 0)
                        r->sockets[idx].compatible_mask |= (1U << target_idx);
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
                const char *dirs[4] = {"N", "E", "S", "W"};
                for (int d = 0; d < 4; d++)
                {
                    const char *pv = find_key_in_range(ports_obj, ports_end, dirs[d]);
                    if (pv)
                    {
                        char sname[32];
                        parse_str(pv, sname, sizeof(sname));
                        int sidx = find_socket_idx(r, sname);
                        r->tiles[idx].ports[d] = (sidx >= 0) ? (uint8_t)sidx : 0;
                    }
                }
            }
            cur = obj_end + 1;
        }
    }

    // parse architect seed and vertical anchors
    const char *arch_start = find_key_in_range(buf, end, "architect");
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
        const char *va = find_key_in_range(arch_start, arch_end, "vertical_anchors");
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

                char tname[64];
                const char *v = find_key_in_range(obj, obj_end, "lower");
                if (v) { parse_str(v, tname, sizeof(tname)); int ti = find_tile_idx(r, tname); r->anchors[aidx].lower_tile_id = (ti >= 0) ? ti : 0; }
                v = find_key_in_range(obj, obj_end, "upper");
                if (v) { parse_str(v, tname, sizeof(tname)); int ti = find_tile_idx(r, tname); r->anchors[aidx].upper_tile_id = (ti >= 0) ? ti : 0; }
                v = find_key_in_range(obj, obj_end, "density_divisor");
                r->anchors[aidx].density_divisor = v ? (uint32_t)strtoul(v, NULL, 10) : 400;
                v = find_key_in_range(obj, obj_end, "min_per_layer");
                r->anchors[aidx].min_per_layer = v ? (uint32_t)strtoul(v, NULL, 10) : 2;

                cur = obj_end + 1;
            }
        }
    }

    free(buf);
    cgsme_ruleset_build_tables(r);
    return (r->num_tiles > 0);
}