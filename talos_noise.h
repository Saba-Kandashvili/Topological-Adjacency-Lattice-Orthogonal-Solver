#ifndef TALOS_NOISE_H
#define TALOS_NOISE_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <math.h>
#include "tiles.h"

typedef struct
{
	uint16_t x;
	uint16_t y;
	float score; // ridged noise value
} PixelData;

int comparePixels(const void *a, const void *b);
float getValueNoise(float x, float y, uint32_t seed);
int measureRegionSize(talos_tile_t ***grid, bool *visited, int width, int length, int startX, int startY);
void keepOnlyLargestMask(talos_tile_t ***grid, int width, int length, int height, int startX, int startY);
int dilateMask(talos_tile_t ***grid, int width, int length, int height, int maxToAdd);
void generateRidgedMask(talos_tile_t ***grid, uint32_t width, uint32_t length, uint32_t height, uint32_t targetFullness, uint32_t seed);
void saveNoiseDebug(float *noiseMap, uint32_t width, uint32_t length);
void saveBinaryMaskDebug(talos_tile_t ***grid, uint32_t width, uint32_t length);

#endif // TALOS_NOISE_H
