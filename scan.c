#include "scan.h"

#include "generator.h"
#include "finders.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MC_VERSION  MC_1_21   /* check generator.h for the exact enum */
#define SURFACE_Y   60        /* height for surface biome lookups */
#define CAVE_Y     (-50)      /* height for cave biome lookups */

/* ---- helpers --------------------------------------------------------- */

/* Biomes since 1.18 live in 4x4x4 cells. getBiomeAt() with scale=4 wants
 * coordinates already divided by 4. Shifting right by 2 does that, and unlike
 * integer division it rounds the right way for negative numbers (-3 >> 2 is
 * -1, whereas -3 / 4 is 0). Getting this wrong is the classic silent bug. */
static int biome_at_block(Generator *g, int bx, int by, int bz)
{
    return getBiomeAt(g, 4, bx >> 2, by >> 2, bz >> 2);
}

static int in_list(int id, const int *list, int n)
{
    for (int i = 0; i < n; i++)
        if (list[i] == id) return 1;
    return 0;
}

/* Every ocean biome, used for "is this land?" tests regardless of which ocean
 * types the user allows around their island. */
static int is_ocean(int id)
{
    switch (id) {
    case ocean: case deep_ocean: case cold_ocean: case deep_cold_ocean:
    case lukewarm_ocean: case deep_lukewarm_ocean: case warm_ocean:
    case frozen_ocean: case deep_frozen_ocean:
        return 1;
    default:
        return 0;
    }
}

/* Whether a biome counts as ocean for the isolation rings. With no ocean types
 * chosen, any ocean qualifies; otherwise only the chosen ones do. */
static int is_ring_ocean(const ScanConfig *cfg, int id)
{
    if (cfg->nOcean == 0) return is_ocean(id);
    return in_list(id, cfg->ocean, cfg->nOcean);
}

/* Counts how many of `samples` points on a circle of `radius` are ring-ocean. */
static int ocean_ring(Generator *g, const ScanConfig *cfg, int radius, int samples)
{
    int hits = 0;
    for (int i = 0; i < samples; i++) {
        double a = 2.0 * M_PI * i / samples;
        int x = (int)(cos(a) * radius);
        int z = (int)(sin(a) * radius);
        if (is_ring_ocean(cfg, biome_at_block(g, x, SURFACE_Y, z)))
            hits++;
    }
    return hits;
}

/* Flood-fills the land connected to spawn (the origin) across a square window
 * and decides whether that land blob is entirely ringed by ocean. Returns 1 if
 * enclosed, 0 otherwise; writes the blob's cell count to *cells.
 *
 * Unlike an ocean-ring percentage, this catches a peninsula: a land bridge of
 * any width joins the blob to the mainland, the fill follows it to the window
 * edge, and touching the edge means "land continues beyond what we can see" ->
 * not enclosed. 8-connectivity is used so even a diagonal bridge is followed. */
static int island_enclosed(Generator *g, int window, int step, int minCells,
                           int *cells)
{
    *cells = 0;
    if (window <= 0 || step <= 0)
        return 1;                       /* check disabled */

    int n = 2 * (window / step) + 1;    /* odd, so the centre cell is spawn */
    int c = n / 2;

    unsigned char *land = malloc((size_t)n * n);
    int *stack = malloc(sizeof(int) * (size_t)n * n);
    if (!land || !stack) { free(land); free(stack); return 0; }

    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) {
            int bx = (i - c) * step;
            int bz = (j - c) * step;
            land[j * n + i] = is_ocean(biome_at_block(g, bx, SURFACE_Y, bz)) ? 0 : 1;
        }

    int enclosed = 1, area = 0, top = 0;
    if (land[c * n + c]) {              /* spawn is land (it should be) */
        land[c * n + c] = 2;
        stack[top++] = c * n + c;
    }
    while (top > 0) {
        int idx = stack[--top];
        int i = idx % n, j = idx / n;
        area++;
        if (i == 0 || j == 0 || i == n - 1 || j == n - 1)
            enclosed = 0;              /* blob reaches the edge -> a bridge out */
        for (int dj = -1; dj <= 1; dj++)
            for (int di = -1; di <= 1; di++) {
                if (!di && !dj) continue;
                int ni = i + di, nj = j + dj;
                if (ni < 0 || nj < 0 || ni >= n || nj >= n) continue;
                int nidx = nj * n + ni;
                if (land[nidx] == 1) {
                    land[nidx] = 2;
                    stack[top++] = nidx;
                }
            }
    }

    free(land);
    free(stack);
    *cells = area;
    if (!enclosed) return 0;
    if (minCells > 0 && area < minCells) return 0;
    return 1;
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

ScanResult scanner_check(void *s, uint64_t seed, const ScanConfig *cfg)
{
    Generator *g = (Generator *)s;
    ScanResult r;
    memset(&r, 0, sizeof(r));

    applySeed(g, DIM_OVERWORLD, seed);

    /* 1. Inner ocean ring. Cheapest test, so it goes first. */
    if (cfg->ringSamples > 0 &&
        ocean_ring(g, cfg, cfg->ringRadius, cfg->ringSamples) < cfg->ringMinOcean)
        return r;

    /* 2. Land in the middle -- otherwise it is just open sea. */
    if (is_ocean(biome_at_block(g, 0, SURFACE_Y, 0)))
        return r;

    /* 3. Outer isolation ring, if enabled. */
    if (cfg->outerSamples > 0 &&
        ocean_ring(g, cfg, cfg->outerRadius, cfg->outerSamples) < cfg->outerMinOcean)
        return r;

    /* 4. One pass over the island footprint, gathering which desired surface
     *    biomes appear and how many sample points hit each desired cave biome. */
    unsigned surfaceSeen = 0;                 /* bit i = cfg->surface[i] seen */
    int caveHits[SCAN_MAX_LIST];
    memset(caveHits, 0, sizeof(caveHits));
    int caveTotal = 0;
    int landCount = 0, sampleCount = 0;
    int step = cfg->gridStep > 0 ? cfg->gridStep : 16;

    for (int x = -cfg->islandRadius; x <= cfg->islandRadius; x += step) {
        for (int z = -cfg->islandRadius; z <= cfg->islandRadius; z += step) {
            if (x * x + z * z > cfg->islandRadius * cfg->islandRadius)
                continue;   /* circle, not square */

            int surf = biome_at_block(g, x, SURFACE_Y, z);
            sampleCount++;
            if (!is_ocean(surf)) landCount++;

            if (cfg->nSurface > 0) {
                for (int i = 0; i < cfg->nSurface; i++)
                    if (cfg->surface[i] == surf) surfaceSeen |= (1u << i);
            }
            if (cfg->nCave > 0) {
                int c = biome_at_block(g, x, CAVE_Y, z);
                for (int i = 0; i < cfg->nCave; i++)
                    if (cfg->cave[i] == c) { caveHits[i]++; caveTotal++; }
            }
        }
    }

    /* Landmass size floor: enough of the footprint must be land. */
    if (cfg->minLandPercent > 0 && sampleCount > 0 &&
        landCount * 100 < cfg->minLandPercent * sampleCount)
        return r;

    /* Surface biome requirement. */
    if (cfg->nSurface > 0) {
        unsigned full = (cfg->nSurface >= 32) ? ~0u : ((1u << cfg->nSurface) - 1);
        if (cfg->matchAllSurface) {
            if (surfaceSeen != full) return r;
        } else {
            if (surfaceSeen == 0) return r;
        }
    }

    /* Cave biome requirement. */
    if (cfg->nCave > 0) {
        if (cfg->matchAllCave) {
            for (int i = 0; i < cfg->nCave; i++)
                if (caveHits[i] < 1) return r;
        } else {
            if (caveTotal < cfg->minCave) return r;
        }
    }
    r.caveCount = caveTotal;

    /* 5. Spawn point -- must land on the island, not in the surrounding sea. */
    Pos p = getSpawn(g);
    if (abs(p.x) > cfg->islandRadius || abs(p.z) > cfg->islandRadius)
        return r;
    if (is_ocean(biome_at_block(g, p.x, SURFACE_Y, p.z)))
        return r;
    r.spawnX = p.x;
    r.spawnZ = p.z;

    /* 5b. Definitive isolation: flood-fill the land connected to spawn and
     *     require it to be fully ringed by ocean, not joined to a mainland by a
     *     land bridge the rings would miss. Expensive, so it runs late. */
    if (cfg->requireEnclosed) {
        int cells = 0;
        if (!island_enclosed(g, cfg->islandWindow, cfg->islandStep,
                             cfg->minIslandCells, &cells))
            return r;
        r.islandCells = cells;
    }

    /* 6. Stronghold containment (continent mode). The nearest of the first-ring
     *    strongholds must sit on land within strongholdMaxDist. Most expensive
     *    check, so it runs last on already-promising seeds. */
    if (cfg->requireStronghold) {
        StrongholdIter sh;
        initFirstStronghold(&sh, MC_VERSION, seed);
        double best = 1e18;
        int bx = 0, bz = 0, onLand = 0;
        for (int i = 0; i < 3; i++) {          /* first ring holds 3 strongholds */
            if (!nextStronghold(&sh, g)) break;
            double d = sqrt((double)sh.pos.x * sh.pos.x +
                            (double)sh.pos.z * sh.pos.z);
            if (d < best) {
                best = d;
                bx = sh.pos.x;
                bz = sh.pos.z;
                onLand = !is_ocean(biome_at_block(g, bx, SURFACE_Y, bz));
            }
        }
        if (best > cfg->strongholdMaxDist || !onLand)
            return r;
        r.hasStronghold = 1;
        r.strongholdX = bx;
        r.strongholdZ = bz;
    }

    r.match = 1;
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
