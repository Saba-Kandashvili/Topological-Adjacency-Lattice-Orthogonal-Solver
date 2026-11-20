// compiles the engine a second time with CGSME_SOFT_MODE=1 (uint64_t carrier + runtime json LUTs)
// separate translation unit so baked generateGrid has zero runtime overhead

#define CGSME_SOFT_MODE 1

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#ifndef _MSC_VER
#include <sys/param.h>
#endif

#include "threadRandom.h"
#include "cgsme_debug.h"
#include "tinycthread/tinycthread.h"
#include "cgsme_soft.h"

CgsmeRuleset g_cgsme_soft_ruleset;

bool cgsme_soft_load_ruleset(const char *json_path)
{
    if (!json_path || !json_path[0])
        return false;
    return cgsme_ruleset_load_from_file(json_path, &g_cgsme_soft_ruleset);
}

const CgsmeRuleset *cgsme_soft_get_ruleset(void)
{
    return &g_cgsme_soft_ruleset;
}

// rename non-static symbols with _Soft so they dont clash with baked build
#define q_init                              q_init_Soft
#define q_push                              q_push_Soft
#define q_pop                               q_pop_Soft
#define q_free                              q_free_Soft
#define initHeap                            initHeap_Soft
#define freeHeap                            freeHeap_Soft
#define swapNodes                           swapNodes_Soft
#define bubbleUp                            bubbleUp_Soft
#define bubbleDown                          bubbleDown_Soft
#define heapInsertOrUpdate                  heapInsertOrUpdate_Soft
#define heapPop                             heapPop_Soft

#define comparePixels                       comparePixels_Soft
#define getValueNoise                       getValueNoise_Soft
#define measureRegionSize                   measureRegionSize_Soft
#define keepOnlyLargestMask                 keepOnlyLargestMask_Soft
#define dilateMask                          dilateMask_Soft
#define generateRidgedMask                  generateRidgedMask_Soft
#define saveNoiseDebug                      saveNoiseDebug_Soft
#define saveBinaryMaskDebug                 saveBinaryMaskDebug_Soft

#define isTileRequired                      isTileRequired_Soft
#define collapseTile                        collapseTile_Soft
#define updateNeighbours                    updateNeighbours_Soft
#define calculateScore                      calculateScore_Soft
#define findBestSeedLocation                findBestSeedLocation_Soft
#define update_spawnrates                   update_spawnrates_Soft
#define exactCellValidMask                  exactCellValidMask_Soft
#define backtrackNeighborhood               backtrackNeighborhood_Soft
#define initChunkHeap3D                     initChunkHeap3D_Soft
#define freeChunkHeap3D                     freeChunkHeap3D_Soft
#define bindChunkHeap3D                     bindChunkHeap3D_Soft
#define chunkHeapInsertOrUpdate3D           chunkHeapInsertOrUpdate3D_Soft
#define chunkHeapPop3D                      chunkHeapPop3D_Soft
#define exactCellValidMask3D                exactCellValidMask3D_Soft
#define updateNeighboursChunk3D             updateNeighboursChunk3D_Soft
#define backtrackChunkNeighborhood3D        backtrackChunkNeighborhood3D_Soft
#define findBestSeedLocationChunk3D         findBestSeedLocationChunk3D_Soft

#define openWallPacked                      openWallPacked_Soft
#define findConnectedRegionsInPlace         findConnectedRegionsInPlace_Soft
#define germanWelderExInPlace               germanWelderExInPlace_Soft
#define germanWelderInPlace                 germanWelderInPlace_Soft
#define sealMazeEdges                       sealMazeEdges_Soft
#define fixupEdges                          fixupEdges_Soft
#define repairMazePortsInPlace              repairMazePortsInPlace_Soft
#define pruneUnreachableComponentsInPlace   pruneUnreachableComponentsInPlace_Soft

#define w                                   w_Soft
#define l                                   l_Soft
#define runArchitect                        runArchitect_Soft
#define generateLayerThread                 generateLayerThread_Soft
#define generateGrid                        generateGrid_Soft
#define freeGrid                            freeGrid_Soft

#include "cgsme_utils.c"
#include "cgsme_noise.c"
#include "cgsme_solver.c"
#include "cgsme_topology.c"
#include "generator.c"
