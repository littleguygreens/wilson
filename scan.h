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
    /* Each selectable feature carries a mode: SEL_INCLUDED (0), SEL_REQUIRED (1)
     * or SEL_EXCLUDED (2). Per category the rule is: every REQUIRED must be
     * present, at least one INCLUDED must be present (if any are listed), and
     * every EXCLUDED must be absent. */

    /* Island surface biomes and their per-biome modes. */
    int surface[SCAN_MAX_LIST];
    int surfaceMode[SCAN_MAX_LIST];
    int nSurface;

    /* Ocean biomes and their modes, applied over the surrounding-sea window like
     * surface biomes: every REQUIRED type must appear, at least one INCLUDED type
     * must appear (if any are listed), and no EXCLUDED type may appear. Isolation
     * geometry is separate -- any ocean counts as water there. */
    int ocean[SCAN_MAX_LIST];
    int oceanMode[SCAN_MAX_LIST];
    int nOcean;

    /* Underground cave biomes and their modes. minCave is the sample-count
     * threshold at which a cave biome counts as "present" for required/included
     * (an excluded cave is rejected on any presence at all). */
    int cave[SCAN_MAX_LIST];
    int caveMode[SCAN_MAX_LIST];
    int nCave;
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

    /* Definitive isolation: flood-fill the land connected to spawn across a
     * window and require that blob never touches the window edge -- i.e. it is
     * entirely ringed by ocean, not a peninsula. islandWindow is the half-size
     * in blocks, islandStep the grid spacing, minIslandCells a floor on the
     * blob's cell count so a lone rock does not count. */
    int requireEnclosed;
    int islandWindow;
    int islandStep;
    int minIslandCells;

    /* Minimum ocean moat: the shortest sea gap from the spawn island to any
     * other landmass must be at least this many blocks. 0 just requires
     * enclosure (any gap). Larger values dial in more isolation (rarer). */
    int minMoat;

    /* Structures (cubiomes StructureType) and their modes. Presence = a viable
     * instance within structRadius blocks of spawn. */
    int structures[SCAN_MAX_LIST];
    int structMode[SCAN_MAX_LIST];
    int nStructures;
    int structRadius;
} ScanConfig;

/* Per-feature selection modes. */
#define SEL_INCLUDED 0
#define SEL_REQUIRED 1
#define SEL_EXCLUDED 2

/* What a single seed check gives back. */
typedef struct {
    int match;          /* 1 if the seed passed every active filter */
    int spawnX;
    int spawnZ;
    int caveCount;      /* desired-cave sample hits inside the island */
    int hasStronghold;  /* 1 if a qualifying stronghold was found */
    int strongholdX;    /* nearest qualifying stronghold, if any */
    int strongholdZ;
    int islandCells;    /* land cells in the spawn-connected component */
    int moatBlocks;     /* sea gap to nearest other land; -1 = none in window */
} ScanResult;

/* The generator is handed back as an opaque pointer (void*). Go stores it and
 * passes it back; it never looks inside. This keeps the cgo boundary tiny. */
void       *scanner_new(void);
void        scanner_free(void *s);

ScanResult  scanner_check(void *s, uint64_t seed, const ScanConfig *cfg);

/* Biome id at a single block coordinate (for the hover tooltip). */
int         scanner_biome(void *s, uint64_t seed, int x, int y, int z);

/* Finds viable instances of one structure type (a cubiomes StructureType) whose
 * position falls in the block box [x0,z0]-[x1,z1]. Writes up to `max` (x,z)
 * pairs into out[2*max]; returns how many were found (may exceed max). */
int         scanner_structures(void *s, uint64_t seed, int structType,
                               int x0, int z0, int x1, int z1, int *out, int max);

/* Fills out[size*size] with biome ids for a square centred on 0,0.
 * step = blocks per pixel. y = block height to sample at. */
void        scanner_biome_grid(void *s, uint64_t seed, int size, int step,
                               int y, int *out);

/* Copies cubiomes' 256-entry RGB palette into out[768]. */
void        scanner_colors(unsigned char *out);

#endif
