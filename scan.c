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

/* Applies the ocean-type tri-state over the whole surrounding sea. Oceans use a
 * whitelist rule, not the "at least one" rule the land biomes use: the sea is
 * entirely ocean, so the point is to constrain *which* ocean types may make it
 * up. If any type is INCLUDED, the sea may contain only INCLUDED or REQUIRED
 * types -- any other ocean type (e.g. frozen you left out) rejects the seed.
 * EXCLUDED types reject on any presence (useful when nothing is included), and
 * every REQUIRED type must actually appear. With no ocean modes set, anything
 * goes. Isolation geometry is separate (any ocean isolates); this is type only.
 * Returns 1 if satisfied. */
static int ocean_area_constraints(Generator *g, const ScanConfig *cfg)
{
    if (cfg->nOcean <= 0)
        return 1;

    int anyIncluded = 0;
    for (int i = 0; i < cfg->nOcean; i++)
        if (cfg->oceanMode[i] == SEL_INCLUDED) { anyIncluded = 1; break; }

    /* Sample the same window the enclosure check uses -- the visible sea around
     * the island. Fall back to the outer isolation radius if it is unset. */
    int window = cfg->islandWindow > 0 ? cfg->islandWindow : cfg->outerRadius;
    int step   = cfg->islandStep   > 0 ? cfg->islandStep   : (cfg->gridStep > 0 ? cfg->gridStep : 16);
    if (window <= 0 || step <= 0)
        return 1;

    int reqSeen[SCAN_MAX_LIST];
    memset(reqSeen, 0, sizeof(reqSeen));

    for (int x = -window; x <= window; x += step)
        for (int z = -window; z <= window; z += step) {
            int b = biome_at_block(g, x, SURFACE_Y, z);
            if (!is_ocean(b)) continue;         /* only ocean cells carry a type */

            int mode = -1, idx = -1;            /* this type's configured mode */
            for (int i = 0; i < cfg->nOcean; i++)
                if (cfg->ocean[i] == b) { mode = cfg->oceanMode[i]; idx = i; break; }

            if (mode == SEL_EXCLUDED)
                return 0;                       /* an excluded type is present */
            if (anyIncluded && mode != SEL_INCLUDED && mode != SEL_REQUIRED)
                return 0;                       /* off the whitelist */
            if (mode == SEL_REQUIRED)
                reqSeen[idx] = 1;
        }

    for (int i = 0; i < cfg->nOcean; i++)
        if (cfg->oceanMode[i] == SEL_REQUIRED && !reqSeen[i])
            return 0;                           /* a required type never appeared */
    return 1;
}

/* Counts how many of `samples` points on a circle of `radius` are ocean. This
 * is the isolation test -- land vs. water -- so any ocean type qualifies. */
static int ocean_ring(Generator *g, const ScanConfig *cfg, int radius, int samples)
{
    (void)cfg;
    int hits = 0;
    for (int i = 0; i < samples; i++) {
        double a = 2.0 * M_PI * i / samples;
        int x = (int)(cos(a) * radius);
        int z = (int)(sin(a) * radius);
        if (is_ocean(biome_at_block(g, x, SURFACE_Y, z)))
            hits++;
    }
    return hits;
}

/* Measures the spawn island across a square window: whether its land blob is
 * complete (does not touch the window edge, i.e. not a peninsula), its cell
 * count, and its "moat" -- the shortest ocean gap in blocks to any OTHER
 * landmass (-1 if no other land lies within the window).
 *
 * Two passes over the same grid: an 8-connected flood fill from spawn marks the
 * island (catching land bridges of any width -- the fill follows a bridge to
 * the edge), then a multi-source BFS outward through ocean looks for the nearest
 * MAINLAND. Other land the BFS meets is classified: if that blob runs off the
 * window edge (continental) or is far larger than the spawn island it counts as
 * mainland and sets the moat; a smaller, self-contained blob is a peer islet and
 * is skipped, so nearby small islands do not shrink the moat. */

/* Flood-fills one land component (value 1) starting at `start`, marking it
 * `mark`. Returns its cell count; sets *touchEdge if it reaches the grid edge. */
static int classify_component(unsigned char *land, int n, int start, int mark,
                              int *touchEdge, int *stack)
{
    int top = 0, area = 0;
    *touchEdge = 0;
    land[start] = (unsigned char)mark;
    stack[top++] = start;
    while (top > 0) {
        int idx = stack[--top], i = idx % n, j = idx / n;
        area++;
        if (i == 0 || j == 0 || i == n - 1 || j == n - 1) *touchEdge = 1;
        for (int dj = -1; dj <= 1; dj++)
            for (int di = -1; di <= 1; di++) {
                if (!di && !dj) continue;
                int ni = i + di, nj = j + dj;
                if (ni < 0 || nj < 0 || ni >= n || nj >= n) continue;
                int k = nj * n + ni;
                if (land[k] == 1) { land[k] = (unsigned char)mark; stack[top++] = k; }
            }
    }
    return area;
}

/* A neighbouring landmass counts as mainland if it extends past the window or is
 * more than this many times the spawn island's area. */
#define MAINLAND_FACTOR 4

static void island_metrics(Generator *g, int window, int step,
                           int *complete, int *cells, int *moat)
{
    *complete = 1;
    *cells = 0;
    *moat = -1;
    if (window <= 0 || step <= 0)
        return;                         /* check disabled */

    int n = 2 * (window / step) + 1;    /* odd, so the centre cell is spawn */
    int c = n / 2;

    /* land: 0 ocean, 1 land, 2 spawn island, 3 classified other land */
    unsigned char *land = malloc((size_t)n * n);
    int *queue = malloc(sizeof(int) * (size_t)n * n);
    int *dist = malloc(sizeof(int) * (size_t)n * n);
    int *cstk = malloc(sizeof(int) * (size_t)n * n);
    if (!land || !queue || !dist || !cstk) {
        free(land); free(queue); free(dist); free(cstk); *complete = 0; return;
    }

    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) {
            int bx = (i - c) * step;
            int bz = (j - c) * step;
            land[j * n + i] = is_ocean(biome_at_block(g, bx, SURFACE_Y, bz)) ? 0 : 1;
        }

    /* Pass 1: flood-fill the spawn island (8-connected). */
    int spawnEdge = 0;
    int area = (land[c * n + c]) ? classify_component(land, n, c * n + c, 2, &spawnEdge, queue) : 0;
    if (spawnEdge) *complete = 0;      /* blob reaches the edge -> a bridge out */
    *cells = area;

    /* Pass 2: BFS out through ocean; classify each landmass met, keep the
     * nearest one that qualifies as mainland. Seed with ocean cells by the
     * island. */
    for (int i = 0; i < n * n; i++) dist[i] = -1;
    int head = 0, tail = 0;
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) {
            if (land[j * n + i] != 2) continue;
            for (int dj = -1; dj <= 1; dj++)
                for (int di = -1; di <= 1; di++) {
                    int ni = i + di, nj = j + dj;
                    if (ni < 0 || nj < 0 || ni >= n || nj >= n) continue;
                    int k = nj * n + ni;
                    if (land[k] == 0 && dist[k] < 0) { dist[k] = 1; queue[tail++] = k; }
                }
        }
    while (head < tail && *moat < 0) {
        int idx = queue[head++];
        int i = idx % n, j = idx / n, d = dist[idx];
        for (int dj = -1; dj <= 1; dj++) {
            for (int di = -1; di <= 1; di++) {
                if (!di && !dj) continue;
                int ni = i + di, nj = j + dj;
                if (ni < 0 || nj < 0 || ni >= n || nj >= n) continue;
                int k = nj * n + ni;
                if (land[k] == 1) {                      /* unclassified landmass */
                    int te, a = classify_component(land, n, k, 3, &te, cstk);
                    if (te || (area > 0 && a > MAINLAND_FACTOR * area)) {
                        *moat = d * step;                /* mainland -> the moat */
                        break;
                    }
                    /* else a peer islet: marked 3, ignored, keep searching */
                } else if (land[k] == 0 && dist[k] < 0) {
                    dist[k] = d + 1;
                    queue[tail++] = k;
                }
            }
            if (*moat >= 0) break;
        }
    }

    free(land);
    free(queue);
    free(dist);
    free(cstk);
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

/* Defined below, in the structures section. */
static int struct_exists(Generator *g, uint64_t seed, int type,
                         int x0, int z0, int x1, int z1);

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

    /* 3b. Ocean-type tri-state across the surrounding sea (required present,
     *     excluded absent, at least one included present). */
    if (!ocean_area_constraints(g, cfg))
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
        int hasInc = 0, incSeen = 0;
        for (int i = 0; i < cfg->nSurface; i++) {
            int present = (surfaceSeen >> i) & 1u;
            switch (cfg->surfaceMode[i]) {
            case SEL_REQUIRED: if (!present) return r; break;
            case SEL_EXCLUDED: if (present) return r; break;
            default:           hasInc = 1; if (present) incSeen = 1; break;
            }
        }
        if (hasInc && !incSeen) return r;
    }

    /* Cave biome requirement. A cave counts as present for required/included at
     * minCave samples; an excluded cave is rejected on any presence. */
    if (cfg->nCave > 0) {
        int thresh = cfg->minCave > 0 ? cfg->minCave : 1;
        int hasInc = 0, incSeen = 0;
        for (int i = 0; i < cfg->nCave; i++) {
            int strong = caveHits[i] >= thresh;
            int any = caveHits[i] >= 1;
            switch (cfg->caveMode[i]) {
            case SEL_REQUIRED: if (!strong) return r; break;
            case SEL_EXCLUDED: if (any) return r; break;
            default:           hasInc = 1; if (strong) incSeen = 1; break;
            }
        }
        if (hasInc && !incSeen) return r;
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
        int complete, cells, moat;
        island_metrics(g, cfg->islandWindow, cfg->islandStep,
                       &complete, &cells, &moat);
        if (!complete)
            return r;                           /* land bridge to a mainland */
        if (cfg->minIslandCells > 0 && cells < cfg->minIslandCells)
            return r;                           /* just a rock */
        /* moat == -1 means no other land in the window (maximally isolated). */
        if (cfg->minMoat > 0 && moat >= 0 && moat < cfg->minMoat)
            return r;                           /* another landmass too close */
        r.islandCells = cells;
        r.moatBlocks = moat;
    }

    /* 5c. Structure requirements: required present, excluded absent, and at
     *     least one included present (if any). */
    if (cfg->nStructures > 0) {
        int R = cfg->structRadius > 0 ? cfg->structRadius : cfg->islandRadius;
        int hasInc = 0, incSeen = 0;
        for (int i = 0; i < cfg->nStructures; i++) {
            int present = struct_exists(g, seed, cfg->structures[i], -R, -R, R, R);
            switch (cfg->structMode[i]) {
            case SEL_REQUIRED: if (!present) return r; break;
            case SEL_EXCLUDED: if (present) return r; break;
            default:           hasInc = 1; if (present) incSeen = 1; break;
            }
        }
        if (hasInc && !incSeen) return r;
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

int scanner_biome(void *s, uint64_t seed, int x, int y, int z)
{
    Generator *g = (Generator *)s;
    applySeed(g, DIM_OVERWORLD, seed);
    return biome_at_block(g, x, y, z);
}

/* ---- structures ------------------------------------------------------ */
/* Note: floordiv() (round toward -inf) comes from cubiomes' rng.h. */

/* Does a viable instance of `type` exist within the block box? Early-out. The
 * generator must already be seeded for the overworld. */
static int struct_exists(Generator *g, uint64_t seed, int type,
                         int x0, int z0, int x1, int z1)
{
    StructureConfig sc;
    if (!getStructureConfig(type, MC_VERSION, &sc)) return 0;
    int reg = sc.regionSize * 16;
    if (reg <= 0) return 0;
    for (int rz = floordiv(z0, reg); rz <= floordiv(z1, reg); rz++)
        for (int rx = floordiv(x0, reg); rx <= floordiv(x1, reg); rx++) {
            Pos p;
            if (!getStructurePos(type, MC_VERSION, seed, rx, rz, &p)) continue;
            if (p.x < x0 || p.x > x1 || p.z < z0 || p.z > z1) continue;
            if (isViableStructurePos(type, g, p.x, p.z, 0)) return 1;
        }
    return 0;
}

int scanner_structures(void *s, uint64_t seed, int structType,
                       int x0, int z0, int x1, int z1, int *out, int max)
{
    Generator *g = (Generator *)s;
    applySeed(g, DIM_OVERWORLD, seed);
    StructureConfig sc;
    if (!getStructureConfig(structType, MC_VERSION, &sc)) return 0;
    int reg = sc.regionSize * 16;
    if (reg <= 0) return 0;
    int cnt = 0;
    for (int rz = floordiv(z0, reg); rz <= floordiv(z1, reg); rz++)
        for (int rx = floordiv(x0, reg); rx <= floordiv(x1, reg); rx++) {
            Pos p;
            if (!getStructurePos(structType, MC_VERSION, seed, rx, rz, &p)) continue;
            if (p.x < x0 || p.x > x1 || p.z < z0 || p.z > z1) continue;
            if (!isViableStructurePos(structType, g, p.x, p.z, 0)) continue;
            if (cnt < max) { out[cnt * 2] = p.x; out[cnt * 2 + 1] = p.z; }
            cnt++;
        }
    return cnt;
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
