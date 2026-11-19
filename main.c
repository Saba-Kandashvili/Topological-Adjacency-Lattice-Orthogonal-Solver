#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "generator.h"
#include "cgsme_soft.h"
#include "cgsme_debug.h"
#include <time.h>

int main(int argc, char **argv)
{
    // parse cli flags
    setbuf(stdout, NULL);
    bool cgsme_debug_enabled = false;
    bool cgsme_quick = false;
    const char *soft_tileset_path = NULL;
    for (int i = 1; i < argc; ++i)
    {
        if (!argv[i])
            continue;
        if (strcmp(argv[i], "--cgsme-debug") == 0 || strcmp(argv[i], "-d") == 0)
            cgsme_debug_enabled = true;
        else if (strcmp(argv[i], "--cgsme-debug-quick") == 0)
            cgsme_quick = true;
        else if ((strcmp(argv[i], "--soft") == 0 || strcmp(argv[i], "-s") == 0) && i + 1 < argc)
            soft_tileset_path = argv[++i];
        else if (strncmp(argv[i], "--soft=", 7) == 0)
            soft_tileset_path = argv[i] + 7;
    }

    if (cgsme_quick)
    {
        cgsme_set_quick_mode(true);
    }
    else
    {
        cgsme_init_debug();
        if (cgsme_debug_enabled)
            cgsme_set_enabled(true);
    }

    // optional profiling thresholds
    uint64_t cgsme_profile_us_th = 0;
    uint64_t cgsme_profile_cycles_th = 0;
    for (int i = 1; i < argc; ++i)
    {
        const char *a = argv[i];
        if (strncmp(a, "--cgsme-profile-threshold-us=", 27) == 0)
        {
            cgsme_profile_us_th = (uint64_t)strtoull(a + 27, NULL, 10);
        }
        else if (strncmp(a, "--cgsme-profile-threshold-cycles=", 33) == 0)
        {
            cgsme_profile_cycles_th = (uint64_t)strtoull(a + 33, NULL, 10);
        }
    }
    if (cgsme_profile_us_th || cgsme_profile_cycles_th)
        cgsme_profile_set_thresholds(cgsme_profile_us_th, cgsme_profile_cycles_th);

    for (int i = 1; i < argc; ++i)
    {
        if (strncmp(argv[i], "--cgsme-profile-warning-pct=", 27) == 0)
        {
            double pct = strtod(argv[i] + 27, NULL);
            if (pct > 0.0)
                cgsme_profile_set_warning_percent(pct);
        }
    }

    // config
    uint32_t width = 25;
    uint32_t length = 25;
    uint32_t height = 10;
    // uint32_t seed = time(NULL);
    uint32_t seed = 5;
    uint32_t fulness = 70;

    printf("Generating %dx%dx%d Maze (Seed: %u, Fullness: %u%%)...\n", width, length, height, seed, fulness);

    if (soft_tileset_path)
    {
        if (!cgsme_soft_load_ruleset(soft_tileset_path))
        {
            fprintf(stderr, "ERROR: [Soft Mode] Failed to load ruleset from '%s'\n", soft_tileset_path);
            return 1;
        }
        const CgsmeRuleset *r = cgsme_soft_get_ruleset();
        printf("[Soft Mode] Loaded ruleset '%s' (%u tiles, universal uint64_t carrier) from %s\n",
               r->name, r->num_tiles, soft_tileset_path);
    }

    // generate
    if (cgsme_quick)
    {
        uint64_t start_us = cgsme_now_us();
        cgsme_tile_t ***grid = NULL;
        uint64_t ***soft_grid = NULL;
        if (soft_tileset_path)
            soft_grid = generateGrid_Soft(width, length, height, seed, fulness);
        else
            grid = generateGrid(width, length, height, seed, fulness);
        uint64_t end_us = cgsme_now_us();

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
        cgsme_tile_t ***grid = NULL;
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
                fprintf(f, "%d,%d,%d\n", width, length, height);

                // z, y, x
                for (int z = 0; z < height; z++)
                {
                    for (int y = 0; y < length; y++)
                    {
                        for (int x = 0; x < width; x++)
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
                printf("Wrote %d layers to maze.txt\n", height);
            }

            if (soft_grid)
                freeGrid_Soft(soft_grid, width, length, height);
            else
                freeGrid(grid, width, length, height);
        }
        else
        {
            printf("Generation Failed.\n");
        }

        cgsme_shutdown_debug();
        return 0;
    }
}