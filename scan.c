#include "scan.h"

#include "generator.h"
#include "finders.h"
#include "util.h"
#include "entries263.h"   /* embedded 26.3 climate entry list (Dappled + Sulfur) */

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

/* Experimental Minecraft 26.3 support. cubiomes has no 26.3 generator, but the
 * decompiled 26.3 world-gen shows the only relevant change is a surgical biome
 * edit on the unchanged 1.18 climate noise: Dappled Forest occupies the plains
 * cell at temperature band 1, humidity band 0, weirdness >= 0. So we keep
 * cubiomes' fast, exact 1.21 generator and relabel that one cell. Verified
 * against chunkbase (seed 1728186647319: dappled at 1250,-100 and 1250,250).
 * Ids beyond cubiomes' enum start at pale_garden(186)+1. */
#define B_DAPPLED_FOREST  187
#define DAP_T_LO  (-4500)   /* temperatures[1] = span(-0.45, -0.15) */
#define DAP_T_HI  (-1500)
#define DAP_H_LO  (-10000)  /* humidities[0]   = span(-1.0, -0.35) */
#define DAP_H_HI  (-3500)

/* Sulfur Caves (26.3): a new underground biome at continentalness [-1900,5500],
 * erosion [4500,10000], depth [2000,9000], weirdness [-11000,-8500]. Unlike
 * Dappled Forest it can't be a simple box test -- its nearest-neighbour
 * territory reaches beyond the box at extreme weirdness -- so we compute the
 * squared distance to the box as a cheap GATE: sulfur can only be the nearest
 * entry where this is small. The box includes depth, so the gate only fires at
 * cave depths, keeping the surface pass fast. */
static long sulfur_box_dist(const int64_t np[6])
{
    static const int lo[6] = { -32768, -32768, -1900,  4500, 2000, -11000 };
    static const int hi[6] = {  32767,  32767,  5500, 10000, 9000,  -8500 };
    long ds = 0;
    for (int i = 0; i < 6; i++) {
        long a = np[i] - hi[i], b = lo[i] - np[i];
        long d = a > 0 ? a : b > 0 ? b : 0;
        ds += d * d;
    }
    return ds;
}
#define SULFUR_GATE 25000000L   /* >2.5x the max distance of any real sulfur cell */

/* Exact nearest-entry biome over the full 26.3 climate table. Only called for
 * the few cave points inside the sulfur gate, to confirm sulfur beats the 1.21
 * winner. Ties resolve to the first (earliest-inserted) entry, matching Mojang. */
static int nearest_263(const int64_t np[6])
{
    long best = -1; int bb = 0;
    for (int k = 0; k < N_ENTRIES_263; k++) {
        const short *e = entries263[k];
        long ds = 0;
        for (int i = 0; i < 6; i++) {
            long a = np[i] - e[2*i+1], b = e[2*i] - np[i];
            long d = a > 0 ? a : b > 0 ? b : 0;
            ds += d * d;
        }
        if (best < 0 || ds < best) { best = ds; bb = e[12]; }
    }
    return bb;
}

/* Biome identity at a block, honouring the 26.3 relabels when exp263 is set:
 * Dappled Forest (cheap surface cell test) and Sulfur Caves (gated exact
 * nearest-entry). Used where the biome *name* matters (island biome gathering,
 * the hover tooltip, the map). Geometry/ocean lookups keep plain biome_at_block
 * so island shape is byte-identical to 1.21. */
static int biome_263(Generator *g, int exp263, int bx, int by, int bz)
{
    if (!exp263)
        return getBiomeAt(g, 4, bx >> 2, by >> 2, bz >> 2);
    int64_t np[6];
    int b = sampleBiomeNoise(&g->bn, np, bx >> 2, by >> 2, bz >> 2, NULL, 0);
    if (b == plains &&
        np[NP_TEMPERATURE] >= DAP_T_LO && np[NP_TEMPERATURE] <= DAP_T_HI &&
        np[NP_HUMIDITY]    >= DAP_H_LO && np[NP_HUMIDITY]    <= DAP_H_HI &&
        np[NP_WEIRDNESS]   >= 0)
        return B_DAPPLED_FOREST;
    if (sulfur_box_dist(np) <= SULFUR_GATE && nearest_263(np) == B_SULFUR_CAVES)
        return B_SULFUR_CAVES;
    return b;
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

/* River biomes. They run through land as narrow channels; wilson normally treats
 * them as land (only true ocean isolates an island), but the dividing-river check
 * needs to tell them apart. */
static int is_river(int id)
{
    return id == river || id == frozen_river;
}

/* Applies the ocean-type tri-state over the whole surrounding sea. Oceans use a
 * whitelist rule, not the "at least one" rule the land biomes use: the sea is
 * entirely ocean, so the point is to constrain *which* ocean types may make it
 * up. If any type is listed as INCLUDED or REQUIRED, that set is the allowed
 * palette -- any other ocean type (e.g. frozen you left out) rejects the seed.
 * EXCLUDED types reject on any presence (useful when nothing is included), and
 * every REQUIRED type must actually appear. With no ocean modes set, anything
 * goes. Isolation geometry is separate (any ocean isolates); this is type only.
 * Returns 1 if satisfied. */
static int ocean_area_constraints(Generator *g, const ScanConfig *cfg)
{
    if (cfg->nOcean <= 0)
        return 1;

    /* The allowed palette is every type marked INCLUDED or REQUIRED. Both narrow
     * the sea to that set, so "all but frozen" keeps frozen out whether you tap
     * those chips to include or to require. */
    int hasAllowed = 0;
    for (int i = 0; i < cfg->nOcean; i++)
        if (cfg->oceanMode[i] == SEL_INCLUDED || cfg->oceanMode[i] == SEL_REQUIRED) { hasAllowed = 1; break; }

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
            if (hasAllowed && mode != SEL_INCLUDED && mode != SEL_REQUIRED)
                return 0;                       /* off the whitelist palette */
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

/* Detects a river that cuts the spawn island clean through, sea-to-sea, so it
 * reads as two islands. The island is the non-ocean blob 8-connected to spawn
 * (rivers count as part of it, exactly as the enclosure check sees it). Treating
 * river cells as barriers, we re-flood the land-only cells from spawn and measure
 * the largest chunk of island land left unreachable: a river running fully across
 * strands one side, while a river that only poks in from the sea and dead-ends
 * leaves the land joined around its tip. We flag the seed only when the stranded
 * side is a substantial share of the island (a fifth), so a channel shaving a
 * small nub off the coast doesn't reject an otherwise good island. Centred on
 * (0,0), which an earlier check has already confirmed is land. Returns 1 to
 * reject. */
static int river_divides(Generator *g, int window, int step)
{
    if (window <= 0 || step <= 0)
        return 0;
    int n = 2 * (window / step) + 1;
    int c = n / 2;

    unsigned char *cell = malloc((size_t)n * n);   /* 0 ocean, 1 land, 2 river */
    unsigned char *mark = malloc((size_t)n * n);   /* 0 outside, 1 blob, 2 spawn land, 3 counted */
    int *stk = malloc(sizeof(int) * (size_t)n * n);
    if (!cell || !mark || !stk) { free(cell); free(mark); free(stk); return 0; }

    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) {
            int bx = (i - c) * step, bz = (j - c) * step;
            int b = biome_at_block(g, bx, SURFACE_Y, bz);
            cell[j * n + i] = is_ocean(b) ? 0 : (is_river(b) ? 2 : 1);
        }

    if (cell[c * n + c] != 1) {                     /* spawn cell not plain land */
        free(cell); free(mark); free(stk);
        return 0;
    }

    /* Blob: land + river 8-connected to spawn (the island as enclosure sees it).
     * Count its land cells along the way. */
    memset(mark, 0, (size_t)n * n);
    int top = 0, totalLand = 0;
    mark[c * n + c] = 1; stk[top++] = c * n + c;
    while (top > 0) {
        int idx = stk[--top], i = idx % n, j = idx / n;
        if (cell[idx] == 1) totalLand++;
        for (int dj = -1; dj <= 1; dj++)
            for (int di = -1; di <= 1; di++) {
                if (!di && !dj) continue;
                int ni = i + di, nj = j + dj;
                if (ni < 0 || nj < 0 || ni >= n || nj >= n) continue;
                int k = nj * n + ni;
                if (!mark[k] && cell[k] != 0) { mark[k] = 1; stk[top++] = k; }
            }
    }

    /* Main land: blob land cells 8-connected to spawn with rivers as barriers. */
    top = 0; mark[c * n + c] = 2; stk[top++] = c * n + c;
    int mainLand = 1;
    while (top > 0) {
        int idx = stk[--top], i = idx % n, j = idx / n;
        for (int dj = -1; dj <= 1; dj++)
            for (int di = -1; di <= 1; di++) {
                if (!di && !dj) continue;
                int ni = i + di, nj = j + dj;
                if (ni < 0 || nj < 0 || ni >= n || nj >= n) continue;
                int k = nj * n + ni;
                if (mark[k] == 1 && cell[k] == 1) { mark[k] = 2; stk[top++] = k; mainLand++; }
            }
    }

    /* Largest land component stranded on the far side of a river. */
    int worst = 0;
    for (int s = 0; s < n * n && worst * 5 < totalLand; s++) {
        if (mark[s] != 1 || cell[s] != 1) continue;   /* unreached blob land */
        top = 0; mark[s] = 3; stk[top++] = s;
        int size = 1;
        while (top > 0) {
            int idx = stk[--top], i = idx % n, j = idx / n;
            for (int dj = -1; dj <= 1; dj++)
                for (int di = -1; di <= 1; di++) {
                    if (!di && !dj) continue;
                    int ni = i + di, nj = j + dj;
                    if (ni < 0 || nj < 0 || ni >= n || nj >= n) continue;
                    int k = nj * n + ni;
                    if (mark[k] == 1 && cell[k] == 1) { mark[k] = 3; stk[top++] = k; size++; }
                }
        }
        if (size > worst) worst = size;
    }

    free(cell); free(mark); free(stk);
    (void)mainLand;
    return totalLand > 0 && worst * 5 >= totalLand;   /* >= 20% stranded -> reject */
}

/* One pass over the island footprint at gridStep resolution. Counts land over
 * the whole circle for the size floor, then flood-fills the spawn-connected land
 * from the centre and gathers surface/cave biome presence ONLY over that
 * component. That way the biome requirements judge the actual spawn island, not
 * a separate islet that merely falls within the search radius. Uses the same
 * grid spacing as before, so caveHits (and the minCave threshold) keep their
 * meaning. Returns 0 only on allocation failure. */
static int gather_island(Generator *g, const ScanConfig *cfg,
                         unsigned *surfaceSeen, int *caveHits, int *caveTotal,
                         int *landCount, int *sampleCount)
{
    *surfaceSeen = 0;
    memset(caveHits, 0, sizeof(int) * SCAN_MAX_LIST);
    *caveTotal = 0;
    *landCount = 0;
    *sampleCount = 0;

    int R = cfg->islandRadius;
    int step = cfg->gridStep > 0 ? cfg->gridStep : 16;
    if (R <= 0 || step <= 0)
        return 1;

    int n = 2 * (R / step) + 1;         /* odd, centre cell is block (0,0) */
    int c = n / 2;
    unsigned char *land = malloc((size_t)n * n);
    int *stk = malloc(sizeof(int) * (size_t)n * n);
    if (!land || !stk) { free(land); free(stk); return 0; }

    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) {
            int bx = (i - c) * step, bz = (j - c) * step;
            land[j * n + i] = is_ocean(biome_at_block(g, bx, SURFACE_Y, bz)) ? 0 : 1;
        }

    /* Land fraction over the circular footprint -- the landmass size floor. This
     * still counts every landmass in view, matching the earlier behaviour. */
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) {
            int bx = (i - c) * step, bz = (j - c) * step;
            if (bx * bx + bz * bz > R * R)
                continue;
            (*sampleCount)++;
            if (land[j * n + i]) (*landCount)++;
        }

    /* Flood-fill the spawn island (its centre is land by an earlier check). */
    if (land[c * n + c] == 1) {
        int edge = 0;
        classify_component(land, n, c * n + c, 2, &edge, stk);
    }

    /* Gather biome membership over the spawn island's cells only. */
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) {
            if (land[j * n + i] != 2)
                continue;
            int bx = (i - c) * step, bz = (j - c) * step;
            if (bx * bx + bz * bz > R * R)
                continue;
            int surf = biome_263(g, cfg->exp263, bx, SURFACE_Y, bz);
            for (int k = 0; k < cfg->nSurface; k++)
                if (cfg->surface[k] == surf) *surfaceSeen |= (1u << k);
            if (cfg->nCave > 0) {
                int cav = biome_263(g, cfg->exp263, bx, CAVE_Y, bz);
                for (int k = 0; k < cfg->nCave; k++)
                    if (cfg->cave[k] == cav) { caveHits[k]++; (*caveTotal)++; }
            }
        }

    free(land);
    free(stk);
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

/* Defined below, in the structures section. */
static int struct_exists(Generator *g, uint64_t seed, int type, int exp263,
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

    /* 4. Flood-fill the spawn island and gather which desired surface biomes
     *    appear on it, and how many sample points hit each desired cave biome --
     *    all restricted to the spawn-connected land, not other nearby islets. */
    unsigned surfaceSeen = 0;                 /* bit i = cfg->surface[i] seen */
    int caveHits[SCAN_MAX_LIST];
    int caveTotal = 0;
    int landCount = 0, sampleCount = 0;
    if (!gather_island(g, cfg, &surfaceSeen, caveHits, &caveTotal,
                       &landCount, &sampleCount))
        return r;                             /* allocation failure */

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

    /* 5b-2. Reject islands a river cuts sea-to-sea (they look like two islands).
     *       Uses the enclosure window/step, so it needs those set. */
    if (cfg->rejectDividingRiver && cfg->islandWindow > 0 && cfg->islandStep > 0 &&
        river_divides(g, cfg->islandWindow, cfg->islandStep))
        return r;

    /* 5c. Structure requirements: required present, excluded absent, and at
     *     least one included present (if any). */
    if (cfg->nStructures > 0) {
        int R = cfg->structRadius > 0 ? cfg->structRadius : cfg->islandRadius;
        int hasInc = 0, incSeen = 0;
        for (int i = 0; i < cfg->nStructures; i++) {
            int present = struct_exists(g, seed, cfg->structures[i], cfg->exp263, -R, -R, R, R);
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

/* scanner_inspect reports one seed's spawn and (if enclosure params are set) its
 * spawn-island metrics, WITHOUT applying any filters. Used by the "view a seed"
 * feature so any known seed can be opened as a card. */
ScanResult scanner_inspect(void *s, uint64_t seed, const ScanConfig *cfg)
{
    Generator *g = (Generator *)s;
    ScanResult r;
    memset(&r, 0, sizeof(r));
    applySeed(g, DIM_OVERWORLD, seed);

    Pos p = getSpawn(g);
    r.spawnX = p.x;
    r.spawnZ = p.z;

    if (cfg->requireEnclosed && cfg->islandWindow > 0 && cfg->islandStep > 0) {
        int complete, cells, moat;
        island_metrics(g, cfg->islandWindow, cfg->islandStep, &complete, &cells, &moat);
        r.islandCells = cells;
        r.moatBlocks = moat;
    }
    r.match = 1;
    return r;
}

int scanner_biome(void *s, uint64_t seed, int x, int y, int z, int exp263)
{
    Generator *g = (Generator *)s;
    applySeed(g, DIM_OVERWORLD, seed);
    return biome_263(g, exp263, x, y, z);
}

/* ---- structures ------------------------------------------------------ */
/* Note: floordiv() (round toward -inf) comes from cubiomes' rng.h. */

/* Abandoned Camp (26.3). cubiomes has no enum for it, so wilson tags it with a
 * private sentinel type well above cubiomes' FEATURE_NUM. It is a plain
 * minecraft:random_spread structure -- the same linear placement Village uses --
 * so cubiomes' getFeaturePos gives its exact region positions from just
 * {salt, regionSize=spacing, chunkRange=spacing-separation}. Config from the
 * 26.3 datapack (abandoned_camp.json): spacing 37, separation 8, salt 91231127.
 * Viability is its own 18-biome allow-list, checked with biome_263 so Dappled
 * Forest counts when 26.3 mode is on (and, being 26.3-only, the whole structure
 * is gated to exp263 on the Go/UI side). */
#define STRUCT_ABANDONED_CAMP 1000
static const StructureConfig CAMP_CONF = { 91231127, 37, 29, 0, 0, 0.f };

/* The biomes an Abandoned Camp may spawn in (26.3 structure variants). Ids are
 * cubiomes' 1.21 ids; B_DAPPLED_FOREST (187) only ever comes back from biome_263
 * when exp263 is set, so listing it here needs no extra guard. */
static const int CAMP_BIOMES[] = {
    bamboo_jungle, birch_forest, cherry_grove, flower_forest, forest, meadow,
    old_growth_pine_taiga, old_growth_birch_forest, old_growth_spruce_taiga,
    pale_garden, savanna, snowy_taiga, sparse_jungle, swamp, taiga,
    windswept_forest, wooded_badlands, B_DAPPLED_FOREST,
};

static int camp_viable(Generator *g, int exp263, int x, int z)
{
    int b = biome_263(g, exp263, x, SURFACE_Y, z);
    for (int i = 0; i < (int)(sizeof CAMP_BIOMES / sizeof CAMP_BIOMES[0]); i++)
        if (CAMP_BIOMES[i] == b) return 1;
    return 0;
}

/* Region positions and biome viability for one structure type over a block box.
 * Abandoned Camp uses the private CAMP_CONF + camp_viable; every other type is a
 * cubiomes StructureType handled by getStructurePos + isViableStructurePos. The
 * two paths share the same region-grid walk here. Calls back into `hit(x,z)` for
 * each viable position (early-out when it returns 1 is the caller's job). */
static int camp_positions(Generator *g, uint64_t seed, int exp263,
                          int x0, int z0, int x1, int z1, int *out, int max)
{
    int reg = CAMP_CONF.regionSize * 16;
    int cnt = 0;
    for (int rz = floordiv(z0, reg); rz <= floordiv(z1, reg); rz++)
        for (int rx = floordiv(x0, reg); rx <= floordiv(x1, reg); rx++) {
            Pos p = getFeaturePos(CAMP_CONF, seed, rx, rz);
            if (p.x < x0 || p.x > x1 || p.z < z0 || p.z > z1) continue;
            if (!camp_viable(g, exp263, p.x, p.z)) continue;
            if (out && cnt < max) { out[cnt * 2] = p.x; out[cnt * 2 + 1] = p.z; }
            cnt++;
            if (!out) return cnt;          /* existence check: first hit is enough */
        }
    return cnt;
}

/* Shipwrecks generate all over the open ocean, but players want the ones grounded
 * on a survival island's shore -- beached, or so close they've all but run aground.
 * A wreck counts as coastal when it is on land itself or real land lies within
 * SHIP_COAST_DIST blocks. Distances are measured to the shore *biome* boundary at
 * 4-block (biome-cell) resolution, so the threshold is kept deliberately tight:
 * the in-game sand can sit a few blocks either side of the biome edge, and at this
 * range that slop matters. Rivers don't count as the coast -- only real land
 * (non-ocean, non-river). */
#define SHIP_COAST_DIST 4
static int is_land_cell(Generator *g, int x, int z)
{
    int b = biome_at_block(g, x, SURFACE_Y, z);
    return !is_ocean(b) && !is_river(b);
}
static int shipwreck_coastal(Generator *g, int x, int z)
{
    /* Scan a disc at biome-cell resolution and keep the wreck only if land falls
     * within the (Euclidean) threshold -- includes the wreck's own cell. */
    for (int dz = -SHIP_COAST_DIST; dz <= SHIP_COAST_DIST; dz += 4)
        for (int dx = -SHIP_COAST_DIST; dx <= SHIP_COAST_DIST; dx += 4)
            if (dx * dx + dz * dz <= SHIP_COAST_DIST * SHIP_COAST_DIST &&
                is_land_cell(g, x + dx, z + dz))
                return 1;
    return 0;
}

/* Does a viable instance of `type` exist within the block box? Early-out. The
 * generator must already be seeded for the overworld. */
static int struct_exists(Generator *g, uint64_t seed, int type, int exp263,
                         int x0, int z0, int x1, int z1)
{
    if (type == STRUCT_ABANDONED_CAMP)
        return camp_positions(g, seed, exp263, x0, z0, x1, z1, NULL, 0) > 0;

    StructureConfig sc;
    if (!getStructureConfig(type, MC_VERSION, &sc)) return 0;
    int reg = sc.regionSize * 16;
    if (reg <= 0) return 0;
    for (int rz = floordiv(z0, reg); rz <= floordiv(z1, reg); rz++)
        for (int rx = floordiv(x0, reg); rx <= floordiv(x1, reg); rx++) {
            Pos p;
            if (!getStructurePos(type, MC_VERSION, seed, rx, rz, &p)) continue;
            if (p.x < x0 || p.x > x1 || p.z < z0 || p.z > z1) continue;
            if (!isViableStructurePos(type, g, p.x, p.z, 0)) continue;
            if (type == Shipwreck && !shipwreck_coastal(g, p.x, p.z)) continue;
            return 1;
        }
    return 0;
}

int scanner_structures(void *s, uint64_t seed, int structType, int exp263,
                       int x0, int z0, int x1, int z1, int *out, int max)
{
    Generator *g = (Generator *)s;
    applySeed(g, DIM_OVERWORLD, seed);

    if (structType == STRUCT_ABANDONED_CAMP)
        return camp_positions(g, seed, exp263, x0, z0, x1, z1, out, max);

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
            if (structType == Shipwreck && !shipwreck_coastal(g, p.x, p.z)) continue;
            if (cnt < max) { out[cnt * 2] = p.x; out[cnt * 2 + 1] = p.z; }
            cnt++;
        }
    return cnt;
}

/* ---- map rendering support ------------------------------------------- */

void scanner_biome_grid(void *s, uint64_t seed, int size, int step,
                        int y, int exp263, int *out)
{
    Generator *g = (Generator *)s;
    applySeed(g, DIM_OVERWORLD, seed);

    int half = size / 2;
    for (int j = 0; j < size; j++) {
        for (int i = 0; i < size; i++) {
            int bx = (i - half) * step;
            int bz = (j - half) * step;
            out[j * size + i] = biome_263(g, exp263, bx, y, bz);
        }
    }
}

void scanner_colors(unsigned char *out)
{
    unsigned char tbl[256][3];
    initBiomeColors(tbl);
    memcpy(out, tbl, sizeof(tbl));
}
