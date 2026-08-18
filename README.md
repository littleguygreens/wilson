# seedscan

Scans random Minecraft Java Edition seeds for a spawn island with mountains and
lush caves underneath. Filtering is done in C against cubiomes; Go handles the
worker pool, the CLI and the PNG output.

## Build

Requires gcc and Go. Network access needed for the clone.

    cd seedscan
    git clone https://github.com/Cubitect/cubiomes.git
    make -C cubiomes libcubiomes
    go build

## Run

    ./seedscan -n 5 -out matches

Flags:

    -n        stop after this many matches (default 5)
    -out      directory for the PNG maps (default ./matches)
    -workers  concurrent workers (default: CPU count)

## Tuning

Everything worth adjusting lives at the top of `scan.c`:

    RING_RADIUS     how far out the ocean check looks
    RING_MIN_OCEAN  how much of that circle must be ocean (strictness)
    ISLAND_RADIUS   how big an area counts as "the island"
    MIN_LUSH        how much lush caves biome is required underground

## Known limits

- Lush caves is a *biome*. cubiomes cannot confirm a cave was actually carved
  there, so results are candidates, not guarantees. Check finalists in game.
- Mountain height is inferred from peak-type biomes, not real terrain height.
- `MC_VERSION` in scan.c must match an enum in cubiomes' generator.h.
