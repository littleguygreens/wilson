#ifndef SCAN_H
#define SCAN_H

#include <stdint.h>

/* What a single seed check gives back.
 * Plain ints only -- cgo copies simple structs across cleanly, and Go never
 * has to know anything about cubiomes' own types. */
typedef struct {
    int match;      /* 1 if the seed passed every filter, 0 otherwise */
    int spawnX;
    int spawnZ;
    int peakBiome;  /* biome id of the first peak-type biome found */
    int lushCount;  /* how many sample points hit lush caves underground */
} ScanResult;

/* The generator is handed back as an opaque pointer (void*). Go stores it and
 * passes it back; it never looks inside. This keeps the cgo boundary tiny. */
void       *scanner_new(void);
void        scanner_free(void *s);

ScanResult  scanner_check(void *s, uint64_t seed);

/* Fills out[size*size] with biome ids for a square centred on 0,0.
 * step = blocks per pixel. y = block height to sample at. */
void        scanner_biome_grid(void *s, uint64_t seed, int size, int step,
                               int y, int *out);

/* Copies cubiomes' 256-entry RGB palette into out[768]. */
void        scanner_colors(unsigned char *out);

#endif
