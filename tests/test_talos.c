#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "generator.h"
#include "talos_soft.h"
#include "talos_ruleset.h"

static int popcount64(uint64_t x)
{
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll(x);
#else
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (int)((x * 0x0101010101010101ULL) >> 56);
#endif
}

static int ctz64(uint64_t x)
{
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_ctzll(x);
#else
    int n = 0;
    while ((x & 1ULL) == 0ULL && n < 64) { x >>= 1; n++; }
    return n;
#endif
}

static double now_ms(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq = {0};
    LARGE_INTEGER now;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

static bool can_step_vertical(uint64_t loCell, uint64_t hiCell, const TalosRuleset *r)
{
    if (loCell == 0ULL || hiCell == 0ULL)
        return false;
    int tLo = ctz64(loCell);

    // explicit vertical open port (e.g. 3d pipes)
    bool loOpenUp   = ((loCell & r->vert_prop_mask[r->num_tiles][VERT_IDX_DOWN]) == 0ULL);
    bool hiOpenDown = ((hiCell & r->vert_prop_mask[r->num_tiles][VERT_IDX_UP])   == 0ULL);
    if (loOpenUp && hiOpenDown && ((r->vert_prop_mask[tLo][VERT_IDX_UP] & hiCell) != 0ULL))
        return true;

    // stair / vertical connector anchor
    if (r->vert_prop_mask[tLo][VERT_IDX_UP] != r->all_possible &&
        (r->vert_prop_mask[tLo][VERT_IDX_UP] & hiCell) != 0ULL)
        return true;

    return false;
}

static void verify_3d_reachability(
    uint64_t ***grid,
    uint32_t width, uint32_t length, uint32_t height,
    const TalosRuleset *r,
    int *out_traversable, int *out_reached, int *out_unreachable)
{
    static const int dx[4]  = {  0,  1,  0, -1 };
    static const int dy[4]  = { -1,  0,  1,  0 };
    static const int opp[4] = {  2,  3,  0,  1 };

    uint64_t anyHorizPort = r->has_port_mask[0] | r->has_port_mask[1] |
                            r->has_port_mask[2] | r->has_port_mask[3];
    uint64_t anyVertPort  = (~r->vert_prop_mask[r->num_tiles][VERT_IDX_DOWN] |
                             ~r->vert_prop_mask[r->num_tiles][VERT_IDX_UP]) & r->all_possible;
    uint64_t travMask = anyHorizPort | anyVertPort;

    uint32_t layerArea = width * length;
    uint32_t total3D = layerArea * height;
    uint8_t *vis = calloc(total3D, sizeof(uint8_t));
    uint32_t *bfsQ = malloc(total3D * sizeof(uint32_t));

    int traversable = 0;
    int startFlat = -1;

    for (uint32_t z = 0; z < height; z++)
    {
        for (uint32_t y = 0; y < length; y++)
        {
            for (uint32_t x = 0; x < width; x++)
            {
                uint64_t c = grid[z][y][x];
                if (c != 0ULL && popcount64(c) == 1 && (c & travMask) != 0ULL)
                {
                    traversable++;
                    if (startFlat < 0)
                        startFlat = (int)((z * length + y) * width + x);
                }
            }
        }
    }

    int reached = 0;
    if (startFlat >= 0 && vis && bfsQ)
    {
        uint32_t head = 0, tail = 0;
        vis[startFlat] = 1;
        bfsQ[tail++] = (uint32_t)startFlat;

        while (head < tail)
        {
            uint32_t a = bfsQ[head++];
            reached++;

            uint32_t z = a / layerArea;
            uint32_t rem = a % layerArea;
            uint32_t y = rem / width;
            uint32_t x = rem % width;
            uint64_t cellA = grid[z][y][x];

            for (int d = 0; d < 4; d++)
            {
                if ((cellA & r->has_port_mask[d]) == 0ULL)
                    continue;
                int nx = (int)x + dx[d];
                int ny = (int)y + dy[d];
                if (nx < 0 || ny < 0 || (uint32_t)nx >= width || (uint32_t)ny >= length)
                    continue;
                uint32_t b = (z * length + (uint32_t)ny) * width + (uint32_t)nx;
                if (vis[b])
                    continue;
                uint64_t cellB = grid[z][ny][nx];
                if (cellB != 0ULL && (cellB & r->has_port_mask[opp[d]]) != 0ULL)
                {
                    vis[b] = 1;
                    bfsQ[tail++] = b;
                }
            }

            if (z + 1 < height)
            {
                uint32_t b = ((z + 1) * length + y) * width + x;
                if (!vis[b] && can_step_vertical(cellA, grid[z + 1][y][x], r))
                {
                    vis[b] = 1;
                    bfsQ[tail++] = b;
                }
            }

            if (z > 0)
            {
                uint32_t b = ((z - 1) * length + y) * width + x;
                if (!vis[b] && can_step_vertical(grid[z - 1][y][x], cellA, r))
                {
                    vis[b] = 1;
                    bfsQ[tail++] = b;
                }
            }
        }
    }

    free(vis);
    free(bfsQ);

    *out_traversable = traversable;
    *out_reached = reached;
    *out_unreachable = traversable - reached;
}

static void verify_soft_grid(
    uint64_t ***grid,
    uint32_t width, uint32_t length, uint32_t height,
    const TalosRuleset *r,
    int *out_filled, int *out_mismatches, int *out_void_leaks, int *out_uncollapsed)
{
    static const int dx[4]  = {  0,  1,  0, -1 };
    static const int dy[4]  = { -1,  0,  1,  0 };
    static const int opp[4] = {  2,  3,  0,  1 };

    int mismatches = 0;
    int void_leaks = 0;
    int uncollapsed = 0;
    int filled = 0;

    for (uint32_t z = 0; z < height; z++)
    {
        for (uint32_t y = 0; y < length; y++)
        {
            for (uint32_t x = 0; x < width; x++)
            {
                uint64_t cell = grid[z][y][x];
                if (cell == 0ULL)
                    continue;
                int pop = popcount64(cell);
                if (pop != 1)
                {
                    uncollapsed++;
                    continue;
                }
                filled++;
                int t = ctz64(cell);
                for (int d = 0; d < 4; d++)
                {
                    int nx = (int)x + dx[d];
                    int ny = (int)y + dy[d];
                    if (nx < 0 || ny < 0 || (uint32_t)nx >= width || (uint32_t)ny >= length || grid[z][ny][nx] == 0ULL)
                    {
                        if ((r->prop_mask[r->num_tiles][opp[d]] & cell) == 0ULL)
                            void_leaks++;
                    }
                    else if ((d == 1 || d == 2) && popcount64(grid[z][ny][nx]) == 1)
                    {
                        if ((r->prop_mask[t][d] & grid[z][ny][nx]) == 0ULL)
                            mismatches++;
                    }
                }

                if (height > 1)
                {
                    if (z + 1 >= height)
                    {
                        if ((r->vert_prop_mask[r->num_tiles][VERT_IDX_DOWN] & cell) == 0ULL)
                            void_leaks++;
                    }
                    else if (grid[z + 1][y][x] != 0ULL && popcount64(grid[z + 1][y][x]) == 1)
                    {
                        if ((r->vert_prop_mask[t][VERT_IDX_UP] & grid[z + 1][y][x]) == 0ULL)
                            mismatches++;
                    }

                    if (z == 0)
                    {
                        if ((r->vert_prop_mask[r->num_tiles][VERT_IDX_UP] & cell) == 0ULL)
                            void_leaks++;
                    }
                }
            }
        }
    }

    if (height > 1 && r->num_anchors > 0 && r->guarantee_layer_connection)
    {
        uint8_t stair_id = r->anchor_lower[0];
        if (!r->category_natural[r->bit_to_category[stair_id]])
        {
            uint64_t stair_mask = (1ULL << stair_id);
            for (uint32_t z = 0; z + 1 < height; z++)
            {
                int stairs_on_layer = 0;
                for (uint32_t y = 0; y < length; y++)
                {
                    for (uint32_t x = 0; x < width; x++)
                    {
                        if (grid[z][y][x] == stair_mask)
                        {
                            stairs_on_layer++;
                            if ((grid[z + 1][y][x] & r->vert_prop_mask[stair_id][VERT_IDX_UP]) == 0ULL)
                                mismatches++;
                            if (!r->anchor_allow_stacking[0] && z > 0 && grid[z - 1][y][x] == stair_mask)
                                mismatches++;
                        }
                    }
                }
                if (stairs_on_layer == 0)
                    mismatches++;
            }
        }
    }

    *out_filled = filled;
    *out_mismatches = mismatches;
    *out_void_leaks = void_leaks;
    *out_uncollapsed = uncollapsed;
}

static int run_baked_vs_soft_case(uint32_t bw, uint32_t bl, uint32_t bh, uint32_t seed, uint32_t fulness, int runs)
{
    const TalosRuleset *r = talos_soft_get_ruleset();
    double baked_min = 1e9, baked_sum = 0.0, baked_max = 0.0;
    double soft_min  = 1e9, soft_sum  = 0.0, soft_max  = 0.0;
    int total_diffs = 0, mm = 0, vl = 0, uc = 0, filled = 0;
    int total_unreach = 0, last_trav = 0, last_reach = 0;

    // warmup
    talos_tile_t ***wb = generateGrid(bw, bl, bh, seed, fulness);
    uint64_t ***ws = generateGrid_Soft(bw, bl, bh, seed, fulness);
    freeGrid(wb, bw, bl, bh);
    freeGrid_Soft(ws, bw, bl, bh);

    for (int i = 0; i < runs; i++)
    {
        uint32_t cur_seed = seed + (uint32_t)i;

        double t0 = now_ms();
        talos_tile_t ***g_baked = generateGrid(bw, bl, bh, cur_seed, fulness);
        double b_ms = now_ms() - t0;

        t0 = now_ms();
        uint64_t ***g_soft = generateGrid_Soft(bw, bl, bh, cur_seed, fulness);
        double s_ms = now_ms() - t0;

        if (b_ms < baked_min) baked_min = b_ms;
        if (b_ms > baked_max) baked_max = b_ms;
        baked_sum += b_ms;

        if (s_ms < soft_min) soft_min = s_ms;
        if (s_ms > soft_max) soft_max = s_ms;
        soft_sum += s_ms;

        for (uint32_t z = 0; z < bh; z++)
            for (uint32_t y = 0; y < bl; y++)
                for (uint32_t x = 0; x < bw; x++)
                    if ((uint64_t)g_baked[z][y][x] != g_soft[z][y][x])
                        total_diffs++;

        int r_f = 0, r_mm = 0, r_vl = 0, r_uc = 0;
        verify_soft_grid(g_soft, bw, bl, bh, r, &r_f, &r_mm, &r_vl, &r_uc);
        filled = r_f;
        mm += r_mm;
        vl += r_vl;
        uc += r_uc;

        int trav = 0, reach = 0, unreach = 0;
        verify_3d_reachability(g_soft, bw, bl, bh, r, &trav, &reach, &unreach);
        last_trav = trav;
        last_reach = reach;
        total_unreach += unreach;

        freeGrid(g_baked, bw, bl, bh);
        freeGrid_Soft(g_soft, bw, bl, bh);
    }

    double reach_pct = (last_trav > 0) ? (100.0 * (double)last_reach / (double)last_trav) : 100.0;
    printf("  [tileset.json %ux%ux%u | fulness=%3u%% | %d seeds]\n"
           "    Baked : min=%6.2f ms | avg=%6.2f ms | max=%6.2f ms\n"
           "    Soft  : min=%6.2f ms | avg=%6.2f ms | max=%6.2f ms\n"
           "    Check : filled=%6d | diffs=%d | uncollapsed=%d | mismatches=%d | void_leaks=%d | 3d_reach=%6.2f%% (unreach=%d)\n\n",
           bw, bl, bh, fulness, runs,
           baked_min, baked_sum / runs, baked_max,
           soft_min, soft_sum / runs, soft_max,
           filled, total_diffs, uc, mm, vl, reach_pct, total_unreach);

    return total_diffs + mm + vl + uc + total_unreach;
}

static int run_soft_case(const char *json_path, uint32_t w, uint32_t l, uint32_t h, uint32_t fulness, int runs, bool check_3d_reach)
{
    if (!talos_soft_load_ruleset(json_path))
    {
        fprintf(stderr, "failed to load %s\n", json_path);
        return 1;
    }
    const TalosRuleset *r = talos_soft_get_ruleset();
    uint32_t seeds[5] = { 1, 2, 5, 42, 99 };
    if (runs > 5) runs = 5;

    double min_ms = 1e9, sum_ms = 0.0, max_ms = 0.0;
    int total_mm = 0, total_vl = 0, total_uc = 0, last_filled = 0;
    int total_unreach = 0, last_trav = 0, last_reach = 0;

    // warmup
    uint64_t ***gw = generateGrid_Soft(w, l, h, seeds[0], fulness);
    freeGrid_Soft(gw, w, l, h);

    for (int s = 0; s < runs; s++)
    {
        double t0 = now_ms();
        uint64_t ***g = generateGrid_Soft(w, l, h, seeds[s], fulness);
        double dt = now_ms() - t0;

        if (dt < min_ms) min_ms = dt;
        if (dt > max_ms) max_ms = dt;
        sum_ms += dt;

        int f = 0, mm = 0, vl = 0, uc = 0;
        verify_soft_grid(g, w, l, h, r, &f, &mm, &vl, &uc);
        last_filled = f;
        total_mm += mm;
        total_vl += vl;
        total_uc += uc;

        if (check_3d_reach)
        {
            int trav = 0, reach = 0, unreach = 0;
            verify_3d_reachability(g, w, l, h, r, &trav, &reach, &unreach);
            last_trav = trav;
            last_reach = reach;
            total_unreach += unreach;
        }
        freeGrid_Soft(g, w, l, h);
    }

    if (check_3d_reach)
    {
        double reach_pct = (last_trav > 0) ? (100.0 * (double)last_reach / (double)last_trav) : 100.0;
        printf("  [%-25s %ux%ux%u | fulness=%3u%% | %d seeds]\n"
               "    Soft  : min=%6.2f ms | avg=%6.2f ms | max=%6.2f ms\n"
               "    Check : filled=%6d | uncollapsed=%d | mismatches=%d | void_leaks=%d | 3d_reach=%6.2f%% (unreach=%d)\n\n",
               json_path, w, l, h, fulness, runs,
               min_ms, sum_ms / runs, max_ms,
               last_filled, total_uc, total_mm, total_vl, reach_pct, total_unreach);
    }
    else
    {
        printf("  [%-25s %ux%ux%u | fulness=%3u%% | %d seeds]\n"
               "    Soft  : min=%6.2f ms | avg=%6.2f ms | max=%6.2f ms\n"
               "    Check : filled=%6d | uncollapsed=%d | mismatches=%d | void_leaks=%d\n\n",
               json_path, w, l, h, fulness, runs,
               min_ms, sum_ms / runs, max_ms,
               last_filled, total_uc, total_mm, total_vl);
    }

    return total_mm + total_vl + total_uc + (check_3d_reach ? total_unreach : 0);
}

int main(int argc, char **argv)
{
    int runs = 5;
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--quick") == 0)
            runs = 1;
    }

    int failures = 0;

    printf("1. tileset.json (baked vs soft parity + 3d reachability)\n\n");
    if (!talos_soft_load_ruleset("tileset.json"))
    {
        fprintf(stderr, "failed to load tileset.json (run from repo root)\n");
        return 1;
    }
    failures += run_baked_vs_soft_case(200, 200, 5, 5, 100, runs);
    failures += run_baked_vs_soft_case(200, 200, 5, 5, 70,  runs);
    failures += run_baked_vs_soft_case(200, 200, 5, 5, 45,  runs);
    failures += run_baked_vs_soft_case(50,  50,  10, 5, 100, runs);
    failures += run_baked_vs_soft_case(50,  50,  10, 5, 70,  runs);
    failures += run_baked_vs_soft_case(200, 200, 1, 5, 100, runs);
    failures += run_baked_vs_soft_case(200, 200, 1, 5, 70,  runs);
    failures += run_baked_vs_soft_case(400, 400, 1, 5, 100, runs);
    failures += run_baked_vs_soft_case(400, 400, 1, 5, 70,  runs);

    printf("2. tilesets/pipes_3d_v1.json (6d pipes + 3d reachability)\n\n");
    failures += run_soft_case("tilesets/pipes_3d_v1.json", 25,  25,  6,  100, runs, true);
    failures += run_soft_case("tilesets/pipes_3d_v1.json", 25,  25,  6,  70,  runs, true);
    failures += run_soft_case("tilesets/pipes_3d_v1.json", 25,  25,  6,  45,  runs, true);
    failures += run_soft_case("tilesets/pipes_3d_v1.json", 50,  50,  10, 100, runs, true);
    failures += run_soft_case("tilesets/pipes_3d_v1.json", 50,  50,  10, 70,  runs, true);
    failures += run_soft_case("tilesets/pipes_3d_v1.json", 30,  30,  20, 100, runs, true);
    failures += run_soft_case("tilesets/pipes_3d_v1.json", 30,  30,  20, 70,  runs, true);
    failures += run_soft_case("tilesets/pipes_3d_v1.json", 100, 100, 5,  100, runs, true);
    failures += run_soft_case("tilesets/pipes_3d_v1.json", 100, 100, 5,  70,  runs, true);
    failures += run_soft_case("tilesets/pipes_3d_v1.json", 200, 200, 5,  100, runs, true);
    failures += run_soft_case("tilesets/pipes_3d_v1.json", 200, 200, 5,  70,  runs, true);

    printf("TOTAL FAILURES: %d\n", failures);
    return (failures == 0) ? 0 : 1;
}
