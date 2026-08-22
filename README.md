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
- **Island biomes** — tap each biome to cycle its state: **include** (an
  any-of group — at least one included biome must appear), **require** (must
  appear), or **exclude** (must not appear).
- **Presets** — one built-in "Mountain Isle" search, plus up to 3 of your own
  saved in the browser (localStorage). Tap to load a full configuration.
- **Ocean types** — tri-state for the surrounding sea, checked across the whole
  visible sea (not just a ring). Unlike the land biomes, oceans use a
  **whitelist**: since the sea is entirely ocean, the point is which ocean types
  may make it up. Any type set to **include** or **require** forms the allowed
  palette — the sea may contain *only* those, so marking everything but frozen
  (as either) keeps frozen out with no extra step. **require** additionally means
  that type must actually appear. **exclude** = this type must not appear anywhere
  in view (handy when you haven't listed anything to include). Isolation itself is
  separate — any ocean counts as the water that isolates the island.
- **Cave biomes** — same include / require / exclude tri-state as surface
  biomes; a cave counts as present at the minimum-sample threshold.
- **Structures on island** — include / require / exclude for each structure
  (village, pillager outpost, woodland mansion, ocean monument, ruined portal,
  ancient city, trial chamber), checked within the island. Stacking several
  requirements is rare.
- **Show structures on maps** — independent per-type toggles overlay colour-coded
  markers on every result map (hover a marker for its name and coordinates), with
  a legend under each map counting how many of each type are in view.
- **Underground biomes** — a toggle re-renders every map at an underground height
  (a Y slider, default y = -50, the depth the search samples) so cave biomes
  (lush / dripstone / deep dark) show as swaths. Since biomes are 3D, it is a
  single horizontal slice; move the slider to see other depths.
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
coordinates and biome under the cursor.

## Experimental: Minecraft 26.3 (Dappled Forest)

A checkbox in the Search tab enables an experimental **26.3** mode that adds the
new **Dappled Forest** biome. cubiomes has no 26.3 generator, but the decompiled
26.3 world-gen shows the climate noise is unchanged from 1.21 -- Dappled Forest
is a surgical biome edit occupying the plains cell at temperature band 1,
humidity band 0, weirdness >= 0. So wilson keeps cubiomes' fast, exact 1.21
generator and *relabels* just that cell (`surface_biome` in `scan.c`). This was
verified against chunkbase: for seed `1728186647319`, wilson reports Dappled
Forest at (1250, -100) and (1250, 250), matching the map. When the mode is off,
generation is byte-identical to plain 1.21. Other 26.3-only biomes (e.g. sulfur
caves) are not modelled yet.

## Saving finds

Tap **☆ Save** on any result to keep it. The **Saved** tab lists your kept
seeds, each rebuilt as a full interactive card (map, structure overlays,
underground layer, tooltips) — everything is derived from the seed, so nothing
but the seed and a little metadata needs storing. Each saved card has a **note**
field for your own remarks. Saved seeds live in the browser; **Export JSON** writes a portable file you can back up or move to
another machine, and **Import JSON** loads it back. The file is just a list of
finds, so it stays small and human-readable. Nothing is written to disk in this
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
an inner ocean ring, land at spawn, a wider isolation ring, ocean-type
constraints across the surrounding sea, a footprint pass that flood-fills the
spawn-connected island and gathers its surface and cave biomes (so biome
requirements judge that island, not a separate islet within the radius) plus the
land fraction, the spawn point, the definitive enclosure/moat flood fill, and
finally the stronghold check. The search is described by a
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
  only guarantees the spawn island itself has no land bridge out. Surface and
  cave biome requirements are judged over the spawn-connected island only, so a
  biome sitting on such a neighbouring islet neither satisfies a requirement nor
  trips an exclusion.
- `MC_VERSION` in `scan.c` must match an enum in cubiomes' `generator.h`. All
  world generation comes from cubiomes, so wilson can only target versions (and
  biomes) that cubiomes has reverse-engineered. The newest it currently supports
  is `1.21 WD` (the Winter Drop / Pale Garden, 1.21.4), which is what wilson is
  pinned to; later snapshots and their new biomes can't be searched until
  upstream cubiomes adds them.
