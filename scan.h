#ifndef SCAN_H
#define SCAN_H

#include <stdint.h>

/* Upper bound on how many biome ids a single list (surface / ocean / cave) can
 * hold. The UI limits the user well below this. */
#define SCAN_MAX_LIST 16

/* A search is fully described by this config, built on the Go side from the web
 * menu and passed by const pointer into scanner_check. Plain ints only, so it
 * copies across the cgo boundary cleanly. */
typedef struct {
    /* Desired island surface biomes. matchAllSurface: 1 = every listed biome
     * must appear on the island, 0 = at least one. Empty list = no requirement. */
    int surface[SCAN_MAX_LIST];
    int nSurface;
    int matchAllSurface;

    /* Ocean biome ids that count as "ocean" for the isolation rings. Empty list
     * falls back to treating every ocean biome as ocean. */
    int ocean[SCAN_MAX_LIST];
    int nOcean;

    /* Desired underground cave biomes. matchAllCave works like matchAllSurface;
     * minCave is the minimum number of sample points that must hit a desired
     * cave biome (in "any" mode) or each desired biome (in "all" mode). */
    int cave[SCAN_MAX_LIST];
    int nCave;
    int matchAllCave;
    int minCave;

    /* Geometry. Radii in blocks; the rings scale with island size on the Go
     * side. gridStep is the island-scan sample spacing. minLandPercent is the
     * minimum share of footprint samples that must be land, so the size slider
     * actually enforces a landmass of that size. */
    int islandRadius;
    int gridStep;
    int minLandPercent;
    int ringRadius, ringSamples, ringMinOcean;
    int outerRadius, outerSamples, outerMinOcean;

    /* Continent mode: require the nearest first-ring stronghold to sit on land
     * within strongholdMaxDist blocks of spawn. */
    int requireStronghold;
    int strongholdMaxDist;
} ScanConfig;

/* What a single seed check gives back. */
typedef struct {
    int match;          /* 1 if the seed passed every active filter */
    int spawnX;
    int spawnZ;
    int caveCount;      /* desired-cave sample hits inside the island */
    int hasStronghold;  /* 1 if a qualifying stronghold was found */
    int strongholdX;    /* nearest qualifying stronghold, if any */
    int strongholdZ;
} ScanResult;

/* The generator is handed back as an opaque pointer (void*). Go stores it and
 * passes it back; it never looks inside. This keeps the cgo boundary tiny. */
void       *scanner_new(void);
void        scanner_free(void *s);

ScanResult  scanner_check(void *s, uint64_t seed, const ScanConfig *cfg);

/* Fills out[size*size] with biome ids for a square centred on 0,0.
 * step = blocks per pixel. y = block height to sample at. */
void        scanner_biome_grid(void *s, uint64_t seed, int size, int step,
                               int y, int *out);

/* Copies cubiomes' 256-entry RGB palette into out[768]. */
void        scanner_colors(unsigned char *out);

#endif
