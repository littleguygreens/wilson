#include "scan.h"

#include "generator.h"
#include "finders.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- tunables -------------------------------------------------------- */

#define MC_VERSION      MC_1_21   /* check generator.h for the exact enum */

#define RING_RADIUS     250       /* blocks out from origin we test for ocean */
#define RING_SAMPLES    32        /* points around that circle */
#define RING_MIN_OCEAN  30        /* how many must be ocean (30 of 32) */

/* Second, wider ocean ring. The inner ring only proves sea out to 250 blocks,
 * which lets a "match" sit just off a mainland. This outer ring pushes the
 * guaranteed ocean out to OUTER_RADIUS, so hits are genuinely isolated islands.
 * Measured trade-off (fraction of islands that also clear this ring, and the
 * resulting match rate vs the ~1-in-2.3M inner-only baseline):
 *     r=600, >=38/48 (80%)  -> ~33%   ~1 in 7.0M   (the default below)
 *     r=600, >=43/48 (90%)  -> ~16%   ~1 in 14M    (tighter, cleaner ring)
 *     r=700, >=43/48 (90%)  -> ~8%    ~1 in 28M    (very isolated, slow)
 * Raise OUTER_MIN_OCEAN or OUTER_RADIUS for stricter isolation at the cost of
 * rarer hits. */
#define OUTER_RADIUS    600       /* wider ocean ring, for true isolation */
#define OUTER_SAMPLES   48        /* points around the wider circle */
#define OUTER_MIN_OCEAN 38        /* how many must be ocean (~80% of 48) */

#define ISLAND_RADIUS   200       /* area we consider "the island" */
#define GRID_STEP       16        /* sample spacing inside the island */

#define SURFACE_Y       60        /* height for surface biome lookups */
#define CAVE_Y         (-50)      /* height for cave biome lookups */

#define MIN_LUSH        3         /* sample points that must hit lush caves */

/* ---- helpers --------------------------------------------------------- */

/* Biomes since 1.18 live in 4x4x4 cells. getBiomeAt() with scale=4 wants
 * coordinates already divided by 4. Shifting right by 2 does that, and unlike
 * integer division it rounds the right way for negative numbers (-3 >> 2 is
 * -1, whereas -3 / 4 is 0). Getting this wrong is the classic silent bug. */
static int biome_at_block(Generator *g, int bx, int by, int bz)
{
    return getBiomeAt(g, 4, bx >> 2, by >> 2, bz >> 2);
}

static int is_ocean(int id)
{
    switch (id) {
    case ocean:
    case deep_ocean:
    case cold_ocean:
    case deep_cold_ocean:
    case lukewarm_ocean:
    case deep_lukewarm_ocean:
    case warm_ocean:
    case frozen_ocean:
    case deep_frozen_ocean:
        return 1;
    default:
        return 0;
    }
}

static int is_peak(int id)
{
    switch (id) {
    case jagged_peaks:
    case frozen_peaks:
    case stony_peaks:
    case snowy_slopes:
    case windswept_hills:
    case windswept_gravelly_hills:
        return 1;
    default:
        return 0;
    }
}

/* ---- lifecycle ------------------------------------------------------- */

void *scanner_new(void)
{
    Generator *g = malloc(sizeof(Generator));
    if (!g) return NULL;
    setupGenerator(g, MC_VERSION, 0);
    return g;
}

void scanner_free(void *s)
{
    free(s);
}

/* ---- the actual filter ----------------------------------------------- */

ScanResult scanner_check(void *s, uint64_t seed)
{
    Generator *g = (Generator *)s;
    ScanResult r;
    memset(&r, 0, sizeof(r));

    applySeed(g, DIM_OVERWORLD, seed);

    /* 1. Ocean ring. Cheapest test, so it goes first -- it throws away the
     *    overwhelming majority of seeds for 32 lookups. */
    int oceanHits = 0;
    for (int i = 0; i < RING_SAMPLES; i++) {
        double a = 2.0 * M_PI * i / RING_SAMPLES;
        int x = (int)(cos(a) * RING_RADIUS);
        int z = (int)(sin(a) * RING_RADIUS);
        if (is_ocean(biome_at_block(g, x, SURFACE_Y, z)))
            oceanHits++;
    }
    if (oceanHits < RING_MIN_OCEAN)
        return r;

    /* 2. Land in the middle -- otherwise it is just open sea. */
    if (is_ocean(biome_at_block(g, 0, SURFACE_Y, 0)))
        return r;

    /* 2b. Wider ocean ring. Rejects a small island tucked against a mainland,
     *     keeping only genuinely isolated ones. Still far cheaper per rejection
     *     than the island footprint scan below, so it runs before it. */
    int outerOcean = 0;
    for (int i = 0; i < OUTER_SAMPLES; i++) {
        double a = 2.0 * M_PI * i / OUTER_SAMPLES;
        int x = (int)(cos(a) * OUTER_RADIUS);
        int z = (int)(sin(a) * OUTER_RADIUS);
        if (is_ocean(biome_at_block(g, x, SURFACE_Y, z)))
            outerOcean++;
    }
    if (outerOcean < OUTER_MIN_OCEAN)
        return r;

    /* 3 & 4. One pass over the island footprint, collecting both the surface
     *        biome and the cave biome underneath the same point. Doing them
     *        together halves the loop overhead. */
    int peakFound = 0, peakBiome = 0, lush = 0;
    for (int x = -ISLAND_RADIUS; x <= ISLAND_RADIUS; x += GRID_STEP) {
        for (int z = -ISLAND_RADIUS; z <= ISLAND_RADIUS; z += GRID_STEP) {
            if (x * x + z * z > ISLAND_RADIUS * ISLAND_RADIUS)
                continue;   /* circle, not square */

            int b = biome_at_block(g, x, SURFACE_Y, z);
            if (!peakFound && is_peak(b)) {
                peakFound = 1;
                peakBiome = b;
            }
            if (biome_at_block(g, x, CAVE_Y, z) == lush_caves)
                lush++;
        }
    }
    if (!peakFound || lush < MIN_LUSH)
        return r;

    /* 5. Spawn point last -- it is by far the most expensive call here, so it
     *    only ever runs on seeds that already look promising. */
    Pos p = getSpawn(g);
    if (abs(p.x) > ISLAND_RADIUS || abs(p.z) > ISLAND_RADIUS)
        return r;
    if (is_ocean(biome_at_block(g, p.x, SURFACE_Y, p.z)))
        return r;

    r.match     = 1;
    r.spawnX    = p.x;
    r.spawnZ    = p.z;
    r.peakBiome = peakBiome;
    r.lushCount = lush;
    return r;
}

/* ---- map rendering support ------------------------------------------- */

void scanner_biome_grid(void *s, uint64_t seed, int size, int step,
                        int y, int *out)
{
    Generator *g = (Generator *)s;
    applySeed(g, DIM_OVERWORLD, seed);

    int half = size / 2;
    for (int j = 0; j < size; j++) {
        for (int i = 0; i < size; i++) {
            int bx = (i - half) * step;
            int bz = (j - half) * step;
            out[j * size + i] = biome_at_block(g, bx, y, bz);
        }
    }
}

void scanner_colors(unsigned char *out)
{
    unsigned char tbl[256][3];
    initBiomeColors(tbl);
    memcpy(out, tbl, sizeof(tbl));
}
