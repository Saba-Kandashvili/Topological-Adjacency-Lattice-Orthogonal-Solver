#include "cgsme_noise.h"
#include "cgsme_utils.h"
#include "tiles.h"
#include <string.h>

// for sorting, DESCENDING order (high score / best ridge first)
int comparePixels(const void *a, const void *b)
{
    float diff = ((PixelData *)b)->score - ((PixelData *)a)->score; // Note: B - A
    if (diff < 0)
        return -1; // B < A, so A goes first
    if (diff > 0)
        return 1; // B > A, so B goes first
    return 0;
}

// simple, fast deterministic random for noise (squirrel3-ish)
static uint32_t noiseHash(uint32_t n, uint32_t seed)
{
    n += seed;
    n *= 0x1B873593;
    n ^= (n >> 16);
    n *= 0x1B873593;
    n ^= (n >> 16);
    return n;
}

// basic value noise (bilinear interpolation of a random grid)
// returns 0.0 to 1.0
float getValueNoise(float x, float y, uint32_t seed)
{
    uint32_t X = (x >= 0.0f) ? (uint32_t)x : (uint32_t)(int32_t)floorf(x);
    uint32_t Y = (y >= 0.0f) ? (uint32_t)y : (uint32_t)(int32_t)floorf(y);
    float fx = x - (float)X;
    float fy = y - (float)Y;

    // smoothstep for natural transitions
    float sx = fx * fx * (3.0f - 2.0f * fx);
    float sy = fy * fy * (3.0f - 2.0f * fy);

    // hash the 4 corners
    // we normalize the hash (uint32 max) to 0.0-1.0 float
    const float invU32 = 1.0f / 4294967296.0f;
    float n00 = (float)noiseHash(X + Y * 57, seed) * invU32;
    float n10 = (float)noiseHash(X + 1 + Y * 57, seed) * invU32;
    float n01 = (float)noiseHash(X + (Y + 1) * 57, seed) * invU32;
    float n11 = (float)noiseHash(X + 1 + (Y + 1) * 57, seed) * invU32;

    // bilinear interpolation
    float ix0 = n00 + sx * (n10 - n00);
    float ix1 = n01 + sx * (n11 - n01);
    return ix0 + sy * (ix1 - ix0);
}

static int measureRegionSizeWithQueue(
    cgsme_tile_t ***grid, bool *visited, int width, int length, int startX, int startY, Point2D *queue)
{
    int count = 0;
    int head = 0;
    int tail = 0;

    queue[tail++] = (Point2D){startX, startY};
    visited[startY * width + startX] = true;
    count++;

    static const int dx[4] = {0, 0, 1, -1};
    static const int dy[4] = {1, -1, 0, 0};

    while (head < tail)
    {
        Point2D c = queue[head++];

        for (int i = 0; i < 4; i++)
        {
            int nx = c.x + dx[i];
            int ny = c.y + dy[i];

            if (nx < 0 || ny < 0 || nx >= width || ny >= length)
                continue;

            // if it is VALID land (All_Possible) and NOT visited yet
            if (grid[0][ny][nx] != Empty_Tile && !visited[ny * width + nx])
            {
                visited[ny * width + nx] = true;
                queue[tail++] = (Point2D){nx, ny};
                count++;
            }
        }
    }

    return count;
}

// BFS to count the size of a region (non-recursive to avoid stack issues)
int measureRegionSize(cgsme_tile_t ***grid, bool *visited, int width, int length, int startX, int startY)
{
    Point2D *queue = malloc(sizeof(Point2D) * width * length);
    if (!queue)
        return 0;

    int count = measureRegionSizeWithQueue(grid, visited, width, length, startX, startY, queue);
    free(queue);
    return count;
}

// BFS to mark the main area adn delete everythign else
void keepOnlyLargestMask(cgsme_tile_t ***grid, int width, int length, int height, int startX, int startY)
{
    bool *isMain = calloc(width * length, sizeof(bool));
    if (!isMain)
        return;

    Point2D *queue = malloc(sizeof(Point2D) * width * length);
    if (!queue)
    {
        free(isMain);
        return;
    }

    int head = 0, tail = 0;

    queue[tail++] = (Point2D){startX, startY};
    isMain[startY * width + startX] = true;

    int dx[] = {0, 0, 1, -1};
    int dy[] = {1, -1, 0, 0};

    while (head < tail)
    {
        Point2D c = queue[head++];
        for (int i = 0; i < 4; i++)
        {
            int nx = c.x + dx[i];
            int ny = c.y + dy[i];
            if (nx < 0 || ny < 0 || nx >= width || ny >= length)
                continue;

            if (grid[0][ny][nx] != Empty_Tile && !isMain[ny * width + nx])
            {
                isMain[ny * width + nx] = true;
                queue[tail++] = (Point2D){nx, ny};
            }
        }
    }

    // DELETE loop, if not Main, set to Empty
    for (int y = 0; y < length; y++)
    {
        for (int x = 0; x < width; x++)
        {
            if (grid[0][y][x] != Empty_Tile && !isMain[y * width + x])
            {
                for (int z = 0; z < height; z++)
                    grid[z][y][x] = Empty_Tile;
            }
        }
    }
    free(isMain);
    free(queue);
}

// grow the edges of the mask by 1 pixel (dilation)
int dilateMask(cgsme_tile_t ***grid, int width, int length, int height, int maxToAdd)
{
    int added = 0;
    bool *toAdd = calloc(width * length, sizeof(bool));
    if (!toAdd)
        return 0;

    // scan the whole map to find all valid growth spots
    for (int y = 0; y < length; y++)
    {
        for (int x = 0; x < width; x++)
        {
            if (grid[0][y][x] == Empty_Tile) // potential spot to grow
            {
                // if any neighbor is VALID, grow here
                if ((x > 0 && grid[0][y][x - 1] != Empty_Tile) ||
                    (x < width - 1 && grid[0][y][x + 1] != Empty_Tile) ||
                    (y > 0 && grid[0][y - 1][x] != Empty_Tile) ||
                    (y < length - 1 && grid[0][y + 1][x] != Empty_Tile))
                {
                    toAdd[y * width + x] = true;
                }
            }
        }
    }

    for (int y = 0; y < length; y++)
    {
        for (int x = 0; x < width; x++)
        {
            if (toAdd[y * width + x])
            {
                if (added >= maxToAdd)
                {
                    goto done;
                }

                for (int z = 0; z < height; z++)
                    grid[z][y][x] = All_Possible_State;
                added++;
            }
        }
    }

done:
    free(toAdd);
    return added;
}

// layer 0 island filter reusing scratch buffers
static int keepOnlyLargestMaskLayer0(
    cgsme_tile_t ***grid, int width, int length, int startX, int startY, bool *isMain, Point2D *queue)
{
    memset(isMain, 0, (size_t)width * (size_t)length * sizeof(bool));
    int head = 0, tail = 0;
    queue[tail++] = (Point2D){startX, startY};
    isMain[startY * width + startX] = true;

    static const int dx[4] = {0, 0, 1, -1};
    static const int dy[4] = {1, -1, 0, 0};

    while (head < tail)
    {
        Point2D c = queue[head++];
        for (int i = 0; i < 4; i++)
        {
            int nx = c.x + dx[i];
            int ny = c.y + dy[i];
            if (nx < 0 || ny < 0 || nx >= width || ny >= length)
                continue;

            if (grid[0][ny][nx] != Empty_Tile && !isMain[ny * width + nx])
            {
                isMain[ny * width + nx] = true;
                queue[tail++] = (Point2D){nx, ny};
            }
        }
    }

    int kept = 0;
    for (int y = 0; y < length; y++)
    {
        for (int x = 0; x < width; x++)
        {
            if (isMain[y * width + x])
                kept++;
            else if (grid[0][y][x] != Empty_Tile)
                grid[0][y][x] = Empty_Tile;
        }
    }
    return kept;
}

// RIDGED NOISE MASK GENERATION
#define CGSME_NOISE_HIST_BINS 4096

void generateRidgedMask(cgsme_tile_t ***grid, uint32_t width, uint32_t length, uint32_t height, uint32_t targetFullness, uint32_t seed)
{
    CGSME_PROFILE_FUNC();
    printf("Generating Ridged Noise Mask (Fullness: %u%%)...\n", targetFullness);
    printf("  - Target filled pixels: ~%u\n", (uint32_t)((uint64_t)width * (uint64_t)length * (uint64_t)targetFullness / 100));
    printf("  - Grid Size: %ux%u\n", width, length);

    uint32_t totalPixels = width * length;
    uint32_t targetCount = (uint32_t)((uint64_t)totalPixels * (uint64_t)targetFullness / 100);

    // safety floor: ensure at least 20 pixels, BUT do not exceed total pixels
    if (targetCount < 20)
        targetCount = 20;
    if (targetCount > totalPixels)
        targetCount = totalPixels;

    float *scores = malloc(sizeof(float) * totalPixels);
    if (!scores)
        return;

#ifdef cgsme_DEBUG
    float *debugMap = malloc(sizeof(float) * totalPixels);
#endif

    // FREQUENCY
    // more = more branches // TODO: TUNE
    float baseFreq = 12.0f / (float)(width + length);

    // WARP
    // this smears the grid so lines touch each other
    float warpFreq = baseFreq * 0.5f; // warp is usually lower freq than the main noise
    float warpAmp = 4.0f;             // distort coordinates by 4 tiles

    uint32_t hist[CGSME_NOISE_HIST_BINS] = {0};
    uint32_t pIdx = 0;
    for (uint32_t y = 0; y < length; y++)
    {
        float fy = (float)y;
        float wyCoord = fy * warpFreq;
        for (uint32_t x = 0; x < width; x++)
        {
            float fx = (float)x;
            float wxCoord = fx * warpFreq;
            // domain warping
            float q = getValueNoise(wxCoord, wyCoord, seed);
            float r = getValueNoise(wxCoord + 5.2f, wyCoord + 1.3f, seed);

            float wx = fx + (q * warpAmp);
            float wy = fy + (r * warpAmp);

            // ridged noise on warped coordinates
            float n = getValueNoise(wx * baseFreq, wy * baseFreq, seed);

            // The Ridge Math
            float ridge = 1.0f - fabsf((n - 0.5f) * 2.0f);
            ridge = ridge * ridge; // sharpen it (makes lines thinner initially)
            if (ridge < 0.0f) ridge = 0.0f;
            if (ridge > 1.0f) ridge = 1.0f;

            scores[pIdx] = ridge;
            uint32_t bin = (uint32_t)(ridge * (float)(CGSME_NOISE_HIST_BINS - 1));
            if (bin >= CGSME_NOISE_HIST_BINS)
                bin = CGSME_NOISE_HIST_BINS - 1;
            hist[bin]++;

#ifdef cgsme_DEBUG
            debugMap[pIdx] = ridge;
#endif

            pIdx++;
        }
    }

#ifdef cgsme_DEBUG
    saveNoiseDebug(debugMap, width, length);
    free(debugMap);
#endif

    // find cutoff histogram bin so we only sort the border bucket
    uint32_t aboveCount = 0;
    uint32_t cutBin = 0;
    for (int b = CGSME_NOISE_HIST_BINS - 1; b >= 0; b--)
    {
        if (aboveCount + hist[b] >= targetCount)
        {
            cutBin = (uint32_t)b;
            break;
        }
        aboveCount += hist[b];
    }

    uint32_t cutTotal = hist[cutBin];
    uint32_t needFromCut = (targetCount > aboveCount) ? (targetCount - aboveCount) : 0;
    if (needFromCut > cutTotal)
        needFromCut = cutTotal;

    // reset layer 0 and fill everything above cutBin
    uint32_t currentFilled = 0;
    PixelData *cutBuf = (needFromCut > 0 && cutTotal > needFromCut) ? malloc(sizeof(PixelData) * cutTotal) : NULL;
    uint32_t cutIdx = 0;

    pIdx = 0;
    for (uint32_t y = 0; y < length; y++)
    {
        for (uint32_t x = 0; x < width; x++)
        {
            float ridge = scores[pIdx++];
            uint32_t bin = (uint32_t)(ridge * (float)(CGSME_NOISE_HIST_BINS - 1));
            if (bin >= CGSME_NOISE_HIST_BINS)
                bin = CGSME_NOISE_HIST_BINS - 1;

            if (bin > cutBin)
            {
                grid[0][y][x] = All_Possible_State;
                currentFilled++;
            }
            else if (bin == cutBin && cutTotal <= needFromCut && needFromCut > 0)
            {
                grid[0][y][x] = All_Possible_State;
                currentFilled++;
            }
            else
            {
                grid[0][y][x] = Empty_Tile;
                if (bin == cutBin && cutBuf && cutIdx < cutTotal)
                {
                    cutBuf[cutIdx].x = (uint16_t)x;
                    cutBuf[cutIdx].y = (uint16_t)y;
                    cutBuf[cutIdx].score = ridge;
                    cutIdx++;
                }
            }
        }
    }

    if (cutBuf)
    {
        qsort(cutBuf, cutIdx, sizeof(PixelData), comparePixels);
        uint32_t take = (needFromCut < cutIdx) ? needFromCut : cutIdx;
        for (uint32_t i = 0; i < take; i++)
        {
            grid[0][cutBuf[i].y][cutBuf[i].x] = All_Possible_State;
            currentFilled++;
        }
        free(cutBuf);
    }

    free(scores);

    // delete islands
    bool *visited = calloc(totalPixels, sizeof(bool));
    Point2D *bfsQueue = malloc(sizeof(Point2D) * totalPixels);
    if (visited && bfsQueue)
    {
        int maxRegionSize = 0;
        int bestX = -1, bestY = -1;

        for (int y = 0; y < (int)length; y++)
        {
            for (int x = 0; x < (int)width; x++)
            {
                if (grid[0][y][x] != Empty_Tile && !visited[y * (int)width + x])
                {
                    int size = measureRegionSizeWithQueue(grid, visited, (int)width, (int)length, x, y, bfsQueue);
                    if (size > maxRegionSize)
                    {
                        maxRegionSize = size;
                        bestX = x;
                        bestY = y;
                    }
                }
            }
        }

        if (bestX != -1)
            currentFilled = (uint32_t)keepOnlyLargestMaskLayer0(grid, (int)width, (int)length, bestX, bestY, visited, bfsQueue);
    }
    free(visited);
    free(bfsQueue);

    // SAFETY PASS (force minimum thickness)
    // ALWAYS dilate at least once. this turns 1-pixel lines into 3-pixel lines
    // this solves the 2x200 crash

    int added = dilateMask(grid, width, length, 1, 1000000); // dilate layer 0 only, copied to rest below
    currentFilled += (uint32_t)added;

    // RESCUE (dilate back to target)
    int safety = 0;
    while (currentFilled < targetCount && safety < 1000)
    {
        // calulate remaining
        int needed = (int)(targetCount - currentFilled);
        int stepAdded = dilateMask(grid, width, length, 1, needed);
        if (stepAdded == 0)
            break;
        currentFilled += (uint32_t)stepAdded;
        safety++;
    }

    // apply Layer 0 to All layers
    for (uint32_t z = 1; z < height; z++)
    {
        for (uint32_t y = 0; y < length; y++)
            memcpy(grid[z][y], grid[0][y], width * sizeof(cgsme_tile_t));
    }

// DEBUG DUMP
#ifdef cgsme_DEBUG
    saveBinaryMaskDebug(grid, width, length);
#endif
}

// writes the raw noise map to disk for Lua visualization TODO: REMOVE PROBABLY OR MOVE SOMEWHERE
void saveNoiseDebug(float *noiseMap, uint32_t width, uint32_t length)
{
    FILE *f = fopen("debug_noise.txt", "w");
    if (!f)
        return;

    // Header: W,H
    fprintf(f, "%d,%d\n", width, length);

    for (uint32_t y = 0; y < length; y++)
    {
        for (uint32_t x = 0; x < width; x++)
        {
            fprintf(f, "%.4f", noiseMap[y * width + x]);
            if (x < width - 1)
                fprintf(f, ",");
        }
        fprintf(f, "\n");
    }
    fclose(f);
}

// Dumps the binary mask state to disk (0 = Void, 1 = Valid)
void saveBinaryMaskDebug(cgsme_tile_t ***grid, uint32_t width, uint32_t length)
{
    FILE *f = fopen("debug_mask.txt", "w");
    if (!f)
        return;

    // Header
    fprintf(f, "%d,%d\n", width, length);

    for (uint32_t y = 0; y < length; y++)
    {
        for (uint32_t x = 0; x < width; x++)
        {
            // If it is Empty_Tile (0), write 0. Otherwise write 1.
            int val = (grid[0][y][x] == Empty_Tile) ? 0 : 1;
            fprintf(f, "%d", val);

            if (x < width - 1)
                fprintf(f, ",");
        }
        fprintf(f, "\n");
    }
    fclose(f);
}
