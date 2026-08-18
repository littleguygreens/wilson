# wilson

A survival-island seed finder for Minecraft Java Edition (named for the
volleyball -- it hunts for islands to be marooned on). Scans random seeds for
a spawn island matching biome, ocean, cave and size criteria you choose, and
can require the island to be big enough (a "continent") to contain the first
stronghold. Filtering is done in C against cubiomes; Go handles the worker
pool, the web UI, the CLI and the PNG maps.

## Build

Requires gcc and Go. Network access needed for the clone.

    cd wilson
    git clone https://github.com/Cubitect/cubiomes.git
    make -C cubiomes libcubiomes
    go build

## Web UI (control it from your phone)

Run the scanner as a small web server on any machine with the build
toolchain, then drive it from your phone's browser:

    ./wilson -serve

Open `http://<that-machine's-LAN-IP>:8080` on your phone and pick your
parameters:

- **Island size** S → XXL. Bigger sizes push the surrounding ocean farther
  out and zoom the map so the whole landmass fits. **XXL ("continent")** is a
  large isolated landmass rather than a small island.
- **Island biomes** — up to 6 desired surface biomes, with an **All / Any**
  toggle (must contain every one, or at least one).
- **Presets** — one built-in "Mountain Isle" search, plus up to 3 of your own
  saved in the browser (localStorage). Tap to load a full configuration.
- **Allowed ocean types** — which ocean biomes count as the isolating sea
  (none selected = any ocean).
- **Cave biomes** — desired underground biomes, with All / Any and a minimum
  sample count.
- **Structures on island** — require one or more structures (village, pillager
  outpost, woodland mansion, ocean monument, ruined portal, ancient city, trial
  chamber) to have a viable instance within the island. Stacking several is rare.
- **Show structures on maps** — independent per-type toggles overlay colour-coded
  markers on every result map (hover a marker for its name and coordinates), with
  a legend under each map counting how many of each type are in view.
- **Isolation** (always on) — flood-fills the land connected to spawn and
  rejects it if that blob reaches the edge of the search window, i.e. it is
  joined to a mainland by a land bridge. This catches peninsulas of any width
  that an ocean-ring percentage cannot. Separate, unconnected landmasses
  elsewhere in the view are still fine. A **distance-to-mainland** slider sets
  the minimum open-sea gap (moat) between the spawn island and the nearest
  other land; higher values are more isolated and rarer. The search window
  grows to fit the requested moat.
- **Stronghold** — optionally require the nearest first-ring stronghold to sit
  on the island's land within a chosen distance. Realistic only at Huge/XXL,
  since strongholds never generate within ~1,280 blocks of spawn.

Matching seeds stream in live with their biome maps (white cross = spawn, red
pin = stronghold). Tap a seed to copy it. Hover the map to read the block
coordinates and biome under the cursor. Nothing is written to disk in this
mode -- maps are rendered on demand. The page is served from `web/index.html`
(embedded into the binary at build time) and streams over Server-Sent Events,
so a long dry spell still shows live progress.

## CLI

For a one-off scan with the built-in default (mountain + lush-caves island):

    ./wilson -n 5 -out matches

Flags:

    -n        stop after this many matches (default 5)
    -out      directory for the PNG maps (default ./matches)
    -workers  concurrent workers (default: CPU count)
    -serve    run the web UI instead of a one-off CLI scan
    -addr     address for the web UI (default :8080, used with -serve)

## How it works

Each seed runs through cheap-to-expensive stages in `scanner_check` (`scan.c`):
an inner ocean ring, land at spawn, a wider isolation ring, one pass over the
island footprint (surface biomes, cave biomes and land fraction), the spawn
point, and finally the stronghold check. The search is described by a
`ScanConfig` struct built on the Go side from the web menu; the size presets
(radii, sample counts, ocean thresholds, land floor and map zoom) live in
`sizePresets` in `server.go`, and the selectable biomes live in `catalog` in
`main.go`.

## Known limits

- Cave biomes are *biomes*. cubiomes cannot confirm a cave was actually carved
  there, so results are candidates, not guarantees. Check finalists in game.
- Surface "mountains" are inferred from peak-type biomes, not real terrain
  height.
- The isolation flood fill only sees a window around spawn (its half-size
  scales with island size). A genuinely isolated landmass larger than that
  window is rejected, since its ocean border falls outside what we sample.
- Separate, unconnected land can still appear elsewhere on the map; enclosure
  only guarantees the spawn island itself has no land bridge out.
- `MC_VERSION` in `scan.c` must match an enum in cubiomes' `generator.h`.
