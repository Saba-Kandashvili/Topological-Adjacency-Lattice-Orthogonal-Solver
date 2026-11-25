#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "generator.h"
#include "talos_soft.h"
#include "talos_debug.h"
#include <time.h>

static void print_usage(const char *prog)
{
    printf("usage: %s [options]\n"
           "  --size WxLxH       grid dimensions (default: 25x25x10)\n"
           "  -w, --width N      grid width\n"
           "  -l, --length N     grid length\n"
           "  -z, --height N     layer count (also --layers)\n"
           "  --seed N|random    rng seed (default: 5)\n"
           "  -f, --fullness N   target fullness 1..100 (default: 70)\n"
           "  -s, --soft PATH    load tileset json at runtime instead of baked\n"
           "  -d, --talos-debug  enable profiling log\n"
           "  --talos-debug-quick benchmark timing only (no maze.txt write)\n"
           "  --help             show this message\n",
           prog);
}

static bool parse_u32(const char *s, uint32_t *out)
{
    if (!s || !s[0])
        return false;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (!end || *end != '\0')
        return false;
    *out = (uint32_t)v;
    return true;
}

static bool parse_size_arg(const char *s, uint32_t *w, uint32_t *l, uint32_t *h)
{
    unsigned int tw = 0, tl = 0, th = 0;
    int n = sscanf(s, "%ux%ux%u", &tw, &tl, &th);
    if (n != 3)
        n = sscanf(s, "%uX%uX%u", &tw, &tl, &th);
    if (n == 3 && tw >= 4 && tl >= 4 && th >= 1)
    {
        *w = (uint32_t)tw;
        *l = (uint32_t)tl;
        *h = (uint32_t)th;
        return true;
    }
    return false;
}

int main(int argc, char **argv)
{
    // parse cli flags
    setbuf(stdout, NULL);
    bool talos_debug_enabled = false;
    bool talos_quick = false;
    const char *soft_tileset_path = NULL;

    uint32_t width = 25;
    uint32_t length = 25;
    uint32_t height = 10;
    uint32_t seed = 5;
    uint32_t fulness = 70;
    uint64_t talos_profile_us_th = 0;
    uint64_t talos_profile_cycles_th = 0;
    double talos_profile_warn_pct = 0.0;

    for (int i = 1; i < argc; ++i)
    {
        const char *a = argv[i];
        if (!a)
            continue;
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0)
        {
            print_usage(argv[0]);
            return 0;
        }
        else if (strcmp(a, "--talos-debug") == 0 || strcmp(a, "-d") == 0)
            talos_debug_enabled = true;
        else if (strcmp(a, "--talos-debug-quick") == 0)
            talos_quick = true;
        else if ((strcmp(a, "--soft") == 0 || strcmp(a, "-s") == 0) && i + 1 < argc)
            soft_tileset_path = argv[++i];
        else if (strncmp(a, "--soft=", 7) == 0)
            soft_tileset_path = a + 7;
        else if (strcmp(a, "--size") == 0 && i + 1 < argc)
        {
            if (!parse_size_arg(argv[++i], &width, &length, &height))
            {
                fprintf(stderr, "invalid --size value (expected WxLxH, min 4x4x1)\n");
                return 1;
            }
        }
        else if (strncmp(a, "--size=", 7) == 0)
        {
            if (!parse_size_arg(a + 7, &width, &length, &height))
            {
                fprintf(stderr, "invalid --size value (expected WxLxH, min 4x4x1)\n");
                return 1;
            }
        }
        else if ((strcmp(a, "-w") == 0 || strcmp(a, "--width") == 0) && i + 1 < argc)
            parse_u32(argv[++i], &width);
        else if (strncmp(a, "--width=", 8) == 0)
            parse_u32(a + 8, &width);
        else if ((strcmp(a, "-l") == 0 || strcmp(a, "--length") == 0) && i + 1 < argc)
            parse_u32(argv[++i], &length);
        else if (strncmp(a, "--length=", 9) == 0)
            parse_u32(a + 9, &length);
        else if ((strcmp(a, "-z") == 0 || strcmp(a, "--height") == 0 || strcmp(a, "--layers") == 0) && i + 1 < argc)
            parse_u32(argv[++i], &height);
        else if (strncmp(a, "--height=", 9) == 0)
            parse_u32(a + 9, &height);
        else if (strncmp(a, "--layers=", 9) == 0)
            parse_u32(a + 9, &height);
        else if (strcmp(a, "--seed") == 0 && i + 1 < argc)
        {
            const char *val = argv[++i];
            if (strcmp(val, "random") == 0)
                seed = (uint32_t)time(NULL);
            else
                parse_u32(val, &seed);
        }
        else if (strncmp(a, "--seed=", 7) == 0)
        {
            const char *val = a + 7;
            if (strcmp(val, "random") == 0)
                seed = (uint32_t)time(NULL);
            else
                parse_u32(val, &seed);
        }
        else if ((strcmp(a, "-f") == 0 || strcmp(a, "--fullness") == 0 || strcmp(a, "--fulness") == 0) && i + 1 < argc)
            parse_u32(argv[++i], &fulness);
        else if (strncmp(a, "--fullness=", 11) == 0)
            parse_u32(a + 11, &fulness);
        else if (strncmp(a, "--fulness=", 10) == 0)
            parse_u32(a + 10, &fulness);
        else if (strncmp(a, "--talos-profile-threshold-us=", 29) == 0)
            talos_profile_us_th = (uint64_t)strtoull(a + 29, NULL, 10);
        else if (strncmp(a, "--talos-profile-threshold-cycles=", 33) == 0)
            talos_profile_cycles_th = (uint64_t)strtoull(a + 33, NULL, 10);
        else if (strncmp(a, "--talos-profile-warning-pct=", 28) == 0)
            talos_profile_warn_pct = strtod(a + 28, NULL);
    }

    if (fulness > 100)
        fulness = 100;
    if (width < 4 || length < 4 || height < 1)
    {
        fprintf(stderr, "invalid grid size %ux%ux%u (min 4x4x1)\n", width, length, height);
        return 1;
    }

    if (talos_quick)
    {
        talos_set_quick_mode(true);
    }
    else
    {
        talos_init_debug();
        if (talos_debug_enabled)
            talos_set_enabled(true);
    }

    if (talos_profile_us_th || talos_profile_cycles_th)
        talos_profile_set_thresholds(talos_profile_us_th, talos_profile_cycles_th);
    if (talos_profile_warn_pct > 0.0)
        talos_profile_set_warning_percent(talos_profile_warn_pct);

    printf("Generating %ux%ux%u Maze (Seed: %u, Fullness: %u%%)...\n", width, length, height, seed, fulness);

    if (soft_tileset_path)
    {
        if (!talos_soft_load_ruleset(soft_tileset_path))
        {
            fprintf(stderr, "ERROR: [Soft Mode] Failed to load ruleset from '%s'\n", soft_tileset_path);
            return 1;
        }
        const TalosRuleset *r = talos_soft_get_ruleset();
        printf("[Soft Mode] Loaded ruleset '%s' (%u tiles, universal uint64_t carrier) from %s\n",
               r->name, r->num_tiles, soft_tileset_path);
    }

    // generate
    if (talos_quick)
    {
        uint64_t start_us = talos_now_us();
        talos_tile_t ***grid = NULL;
        uint64_t ***soft_grid = NULL;
        if (soft_tileset_path)
            soft_grid = generateGrid_Soft(width, length, height, seed, fulness);
        else
            grid = generateGrid(width, length, height, seed, fulness);
        uint64_t end_us = talos_now_us();

        if (grid || soft_grid)
        {
            double seconds = (double)(end_us - start_us) / 1000000.0;

            // count filled tiles
            uint64_t filled_tiles = 0;
            uint64_t total_tiles = (uint64_t)width * length * height;

            for (uint32_t z = 0; z < height; z++)
            {
                for (uint32_t y = 0; y < length; y++)
                {
                    for (uint32_t x = 0; x < width; x++)
                    {
                        uint64_t cell = soft_grid ? soft_grid[z][y][x] : (uint64_t)grid[z][y][x];
                        if (cell != 0)
                            filled_tiles++;
                    }
                }
            }

            printf("BENCH: generateGrid%s elapsed=%llu us (%.6f s)\n",
                   soft_tileset_path ? "_Soft" : "",
                   (unsigned long long)(end_us - start_us), seconds);
            printf("STATS: Filled %llu / %llu tiles (%.1f%%)\n",
                   (unsigned long long)filled_tiles, (unsigned long long)total_tiles,
                   ((float)filled_tiles / total_tiles) * 100.0f);
            if (soft_grid)
                freeGrid_Soft(soft_grid, width, length, height);
            else
                freeGrid(grid, width, length, height);
        }
        else
        {
            printf("BENCH: generateGrid FAILED (Returned NULL)\n");
        }
        return 0;
    }
    else
    {
        talos_tile_t ***grid = NULL;
        uint64_t ***soft_grid = NULL;
        if (soft_tileset_path)
            soft_grid = generateGrid_Soft(width, length, height, seed, fulness);
        else
            grid = generateGrid(width, length, height, seed, fulness);

        if (grid || soft_grid)
        {
            printf("Generation Complete.\n");

            // dump all layers to maze.txt
            FILE *f = fopen("maze.txt", "w");
            if (f)
            {
                // header: width, length, height
                fprintf(f, "%u,%u,%u\n", width, length, height);

                // z, y, x
                for (uint32_t z = 0; z < height; z++)
                {
                    for (uint32_t y = 0; y < length; y++)
                    {
                        for (uint32_t x = 0; x < width; x++)
                        {
                            uint64_t cell = soft_grid ? soft_grid[z][y][x] : (uint64_t)grid[z][y][x];
                            fprintf(f, "%llu", (unsigned long long)cell);
                            if (x < width - 1)
                                fprintf(f, ",");
                        }
                        fprintf(f, "\n");
                    }
                }
                fclose(f);
                printf("Wrote %u layers to maze.txt\n", height);
            }
            else
            {
                printf("ERROR: Could not open maze.txt for writing!\n");
            }

            if (soft_grid)
                freeGrid_Soft(soft_grid, width, length, height);
            else
                freeGrid(grid, width, length, height);
        }
        else
        {
            printf("ERROR: generateGrid returned NULL!\n");
        }
        talos_shutdown_debug();
    }

    return 0;
}
