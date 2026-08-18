package main

/*
#cgo CFLAGS: -I${SRCDIR}/cubiomes -O3
#cgo LDFLAGS: -L${SRCDIR}/cubiomes -lcubiomes -lm
#include <stdlib.h>
#include "biomes.h"
#include "scan.h"
*/
import "C"

import (
	"context"
	"flag"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"io"
	"log"
	"math/rand/v2"
	"os"
	"path/filepath"
	"runtime"
	"sync"
	"sync/atomic"
	"unsafe"
)

// mapPixels is the width/height of every output map in pixels. The number of
// blocks it spans is mapPixels*step, where step comes from the search size.
const mapPixels = 512

// marker is a block-coordinate point drawn on a map.
type marker struct{ x, z int }

type hit struct {
	seed          uint64
	spawnX        int
	spawnZ        int
	caveCount     int
	hasStronghold bool
	strongholdX   int
	strongholdZ   int
	islandCells   int
	moatBlocks    int
}

// Config is the pure-Go description of a search. It is translated to a C
// ScanConfig by toCConfig; keeping it Go-only lets server.go build searches
// without touching cgo.
type Config struct {
	Surface         []int32
	MatchAllSurface bool
	Ocean           []int32
	Cave            []int32
	MatchAllCave    bool
	MinCave         int

	IslandRadius   int
	GridStep       int
	MinLandPercent int
	RingRadius     int
	RingSamples    int
	RingMinOcean   int
	OuterRadius    int
	OuterSamples   int
	OuterMinOcean  int

	RequireStronghold bool
	StrongholdMaxDist int

	RequireEnclosed bool
	IslandWindow    int
	IslandStep      int
	MinIslandCells  int
	MinMoat         int

	MapStep int // rendering zoom; not part of the C filter
}

// biomeEntry ties a stable UI key to a cubiomes biome id. The id stays
// unexported so the C dependency doesn't leak out of this file.
type biomeEntry struct {
	Category string
	Key      string
	Label    string
	id       int32
}

// catalog is the authoritative list of selectable biomes. The web menu is built
// from it (keys + labels), and search requests map keys back to ids through it.
var catalog = []biomeEntry{
	// Island surface biomes.
	{"surface", "plains", "Plains", int32(C.plains)},
	{"surface", "forest", "Forest", int32(C.forest)},
	{"surface", "birch_forest", "Birch Forest", int32(C.birch_forest)},
	{"surface", "dark_forest", "Dark Forest", int32(C.dark_forest)},
	{"surface", "taiga", "Taiga", int32(C.taiga)},
	{"surface", "snowy_taiga", "Snowy Taiga", int32(C.snowy_taiga)},
	{"surface", "jungle", "Jungle", int32(C.jungle)},
	{"surface", "bamboo_jungle", "Bamboo Jungle", int32(C.bamboo_jungle)},
	{"surface", "savanna", "Savanna", int32(C.savanna)},
	{"surface", "desert", "Desert", int32(C.desert)},
	{"surface", "swamp", "Swamp", int32(C.swamp)},
	{"surface", "mangrove_swamp", "Mangrove Swamp", int32(C.mangrove_swamp)},
	{"surface", "badlands", "Badlands", int32(C.badlands)},
	{"surface", "mushroom_fields", "Mushroom Fields", int32(C.mushroom_fields)},
	{"surface", "cherry_grove", "Cherry Grove", int32(C.cherry_grove)},
	{"surface", "meadow", "Meadow", int32(C.meadow)},
	{"surface", "grove", "Grove", int32(C.grove)},
	{"surface", "snowy_slopes", "Snowy Slopes", int32(C.snowy_slopes)},
	{"surface", "windswept_hills", "Windswept Hills", int32(C.windswept_hills)},
	{"surface", "jagged_peaks", "Jagged Peaks", int32(C.jagged_peaks)},
	{"surface", "frozen_peaks", "Frozen Peaks", int32(C.frozen_peaks)},
	{"surface", "stony_peaks", "Stony Peaks", int32(C.stony_peaks)},

	// Allowed ocean types for the isolation rings.
	{"ocean", "ocean", "Ocean", int32(C.ocean)},
	{"ocean", "deep_ocean", "Deep Ocean", int32(C.deep_ocean)},
	{"ocean", "warm_ocean", "Warm Ocean", int32(C.warm_ocean)},
	{"ocean", "lukewarm_ocean", "Lukewarm Ocean", int32(C.lukewarm_ocean)},
	{"ocean", "deep_lukewarm_ocean", "Deep Lukewarm", int32(C.deep_lukewarm_ocean)},
	{"ocean", "cold_ocean", "Cold Ocean", int32(C.cold_ocean)},
	{"ocean", "deep_cold_ocean", "Deep Cold", int32(C.deep_cold_ocean)},
	{"ocean", "frozen_ocean", "Frozen Ocean", int32(C.frozen_ocean)},
	{"ocean", "deep_frozen_ocean", "Deep Frozen", int32(C.deep_frozen_ocean)},

	// Underground cave biomes.
	{"cave", "lush_caves", "Lush Caves", int32(C.lush_caves)},
	{"cave", "dripstone_caves", "Dripstone Caves", int32(C.dripstone_caves)},
	{"cave", "deep_dark", "Deep Dark", int32(C.deep_dark)},
}

// biomeKeyToID resolves a UI key to its cubiomes biome id.
func biomeKeyToID(key string) (int32, bool) {
	for _, e := range catalog {
		if e.Key == key {
			return e.id, true
		}
	}
	return 0, false
}

// toCConfig marshals a Go Config into the C struct passed to scanner_check.
func toCConfig(cfg Config) C.ScanConfig {
	var c C.ScanConfig
	putList := func(dst *[C.SCAN_MAX_LIST]C.int, src []int32) C.int {
		n := len(src)
		if n > C.SCAN_MAX_LIST {
			n = C.SCAN_MAX_LIST
		}
		for i := 0; i < n; i++ {
			dst[i] = C.int(src[i])
		}
		return C.int(n)
	}
	c.nSurface = putList(&c.surface, cfg.Surface)
	c.nOcean = putList(&c.ocean, cfg.Ocean)
	c.nCave = putList(&c.cave, cfg.Cave)
	c.matchAllSurface = boolToC(cfg.MatchAllSurface)
	c.matchAllCave = boolToC(cfg.MatchAllCave)
	c.minCave = C.int(cfg.MinCave)

	c.islandRadius = C.int(cfg.IslandRadius)
	c.gridStep = C.int(cfg.GridStep)
	c.minLandPercent = C.int(cfg.MinLandPercent)
	c.ringRadius = C.int(cfg.RingRadius)
	c.ringSamples = C.int(cfg.RingSamples)
	c.ringMinOcean = C.int(cfg.RingMinOcean)
	c.outerRadius = C.int(cfg.OuterRadius)
	c.outerSamples = C.int(cfg.OuterSamples)
	c.outerMinOcean = C.int(cfg.OuterMinOcean)

	c.requireStronghold = boolToC(cfg.RequireStronghold)
	c.strongholdMaxDist = C.int(cfg.StrongholdMaxDist)

	c.requireEnclosed = boolToC(cfg.RequireEnclosed)
	c.islandWindow = C.int(cfg.IslandWindow)
	c.islandStep = C.int(cfg.IslandStep)
	c.minIslandCells = C.int(cfg.MinIslandCells)
	c.minMoat = C.int(cfg.MinMoat)
	return c
}

func boolToC(b bool) C.int {
	if b {
		return 1
	}
	return 0
}

func main() {
	wanted := flag.Int("n", 5, "stop after this many matching seeds")
	outDir := flag.String("out", "matches", "directory for PNG maps")
	workers := flag.Int("workers", runtime.NumCPU(), "concurrent workers")
	serve := flag.Bool("serve", false, "run the mobile web UI instead of a one-off CLI scan")
	addr := flag.String("addr", ":8080", "address for the web UI (used with -serve)")
	flag.Parse()

	if *serve {
		log.Fatal(runServer(*addr))
	}

	if err := os.MkdirAll(*outDir, 0o755); err != nil {
		log.Fatal(err)
	}

	cfg := defaultConfig()
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	renderer := C.scanner_new()
	defer C.scanner_free(renderer)

	var tested int64
	found := 0
	runScan(ctx, *workers, &tested, cfg, func(h hit) bool {
		found++
		fmt.Printf("seed %d  spawn %d,%d  caves %d\n",
			int64(h.seed), h.spawnX, h.spawnZ, h.caveCount)

		path := filepath.Join(*outDir, fmt.Sprintf("%d.png", int64(h.seed)))
		if err := writeMapPNG(renderer, h, cfg.MapStep, path); err != nil {
			log.Printf("render %d: %v", int64(h.seed), err)
		}
		return found < *wanted
	})

	fmt.Printf("\ntested %d seeds\n", atomic.LoadInt64(&tested))
}

// defaultConfig is the original "mountains + lush caves island" search, used by
// the CLI so its behaviour is unchanged.
func defaultConfig() Config {
	peaks := []int32{}
	for _, k := range []string{"jagged_peaks", "frozen_peaks", "stony_peaks", "snowy_slopes", "windswept_hills"} {
		if id, ok := biomeKeyToID(k); ok {
			peaks = append(peaks, id)
		}
	}
	lush, _ := biomeKeyToID("lush_caves")

	cfg := geometryForSize("M")
	cfg.Surface = peaks
	cfg.MatchAllSurface = false
	cfg.Cave = []int32{lush}
	cfg.MatchAllCave = false
	cfg.MinCave = 3
	cfg.RequireEnclosed = true
	return cfg
}

// runScan spins up the worker pool and calls onHit for every matching seed, in
// the order they arrive. onHit returns false to stop; runScan also stops when
// ctx is cancelled. It blocks until every worker has exited.
func runScan(parent context.Context, workers int, tested *int64, cfg Config, onHit func(hit) bool) {
	if workers < 1 {
		workers = 1
	}

	// One C config shared read-only by every worker. It contains no Go
	// pointers, so passing its address into C is safe under cgo's rules.
	cCfg := toCConfig(cfg)

	ctx, cancel := context.WithCancel(parent)
	defer cancel()

	hits := make(chan hit)
	var wg sync.WaitGroup
	for i := 0; i < workers; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			search(ctx, hits, tested, &cCfg)
		}()
	}

	go func() {
		wg.Wait()
		close(hits)
	}()

	stopped := false
	for h := range hits {
		if stopped {
			continue
		}
		if !onHit(h) {
			stopped = true
			cancel()
		}
	}
}

func search(ctx context.Context, hits chan<- hit, tested *int64, cCfg *C.ScanConfig) {
	// One generator per goroutine. cubiomes' Generator struct holds mutable
	// state that applySeed() overwrites, so two goroutines sharing one would
	// silently corrupt each other's results -- no crash, just wrong answers.
	s := C.scanner_new()
	defer C.scanner_free(s)

	rng := rand.NewPCG(rand.Uint64(), rand.Uint64())
	r := rand.New(rng)

	for {
		select {
		case <-ctx.Done():
			return
		default:
		}

		seed := r.Uint64()
		res := C.scanner_check(s, C.uint64_t(seed), cCfg)
		atomic.AddInt64(tested, 1)

		if res.match == 0 {
			continue
		}
		select {
		case hits <- hit{
			seed:          seed,
			spawnX:        int(res.spawnX),
			spawnZ:        int(res.spawnZ),
			caveCount:     int(res.caveCount),
			hasStronghold: res.hasStronghold != 0,
			strongholdX:   int(res.strongholdX),
			strongholdZ:   int(res.strongholdZ),
			islandCells:   int(res.islandCells),
			moatBlocks:    int(res.moatBlocks),
		}:
		case <-ctx.Done():
			return
		}
	}
}

// palette pulls cubiomes' own biome colours across once, cached for reuse.
var (
	paletteOnce sync.Once
	paletteData [256]color.RGBA
)

func palette() [256]color.RGBA {
	paletteOnce.Do(func() {
		var raw [768]C.uchar
		C.scanner_colors((*C.uchar)(unsafe.Pointer(&raw[0])))
		for i := 0; i < 256; i++ {
			paletteData[i] = color.RGBA{
				R: uint8(raw[i*3+0]),
				G: uint8(raw[i*3+1]),
				B: uint8(raw[i*3+2]),
				A: 255,
			}
		}
	})
	return paletteData
}

// renderImage draws the biome map for a seed at the given block-per-pixel step,
// marks spawn, and (if present) marks the stronghold.
func renderImage(s unsafe.Pointer, seed uint64, step int, spawn marker, sh *marker) *image.RGBA {
	if step < 1 {
		step = 4
	}
	grid := make([]C.int, mapPixels*mapPixels)
	C.scanner_biome_grid(s, C.uint64_t(seed), mapPixels, C.int(step), C.int(60),
		(*C.int)(unsafe.Pointer(&grid[0])))

	p := palette()
	img := image.NewRGBA(image.Rect(0, 0, mapPixels, mapPixels))
	for j := 0; j < mapPixels; j++ {
		for i := 0; i < mapPixels; i++ {
			id := int(grid[j*mapPixels+i])
			if id < 0 || id > 255 {
				id = 0
			}
			img.SetRGBA(i, j, p[id])
		}
	}

	half := mapPixels / 2
	if sh != nil {
		// Stronghold: a white-bordered red diamond. The white ring keeps it from
		// blending into pink/magenta biomes (mushroom fields, cherry grove).
		drawMarker(img, half+sh.x/step, half+sh.z/step, 6,
			color.RGBA{230, 40, 40, 255})
	}
	// Spawn: a bold white cross with a dark halo, drawn last so it stays on top.
	drawCross(img, half+spawn.x/step, half+spawn.z/step, 7,
		color.RGBA{255, 255, 255, 255})
	return img
}

// drawCross plots a thick cross with a black halo so it reads on any terrain.
func drawCross(img *image.RGBA, cx, cz, arm int, c color.RGBA) {
	black := color.RGBA{0, 0, 0, 255}
	for d := -arm - 1; d <= arm+1; d++ {
		for t := -2; t <= 2; t++ {
			img.SetRGBA(cx+d, cz+t, black)
			img.SetRGBA(cx+t, cz+d, black)
		}
	}
	for d := -arm; d <= arm; d++ {
		for t := -1; t <= 1; t++ {
			img.SetRGBA(cx+d, cz+t, c)
			img.SetRGBA(cx+t, cz+d, c)
		}
	}
}

// drawMarker plots a filled diamond as a colored core inside a white ring inside
// a black halo, so it reads as a map pin against any biome.
func drawMarker(img *image.RGBA, cx, cz, radius int, c color.RGBA) {
	black := color.RGBA{0, 0, 0, 255}
	white := color.RGBA{255, 255, 255, 255}
	fill := func(rad int, col color.RGBA) {
		for dz := -rad; dz <= rad; dz++ {
			for dx := -rad; dx <= rad; dx++ {
				if abs(dx)+abs(dz) <= rad {
					img.SetRGBA(cx+dx, cz+dz, col)
				}
			}
		}
	}
	fill(radius+2, black)
	fill(radius+1, white)
	fill(radius, c)
}

func abs(v int) int {
	if v < 0 {
		return -v
	}
	return v
}

func hitMarkers(h hit) (marker, *marker) {
	spawn := marker{h.spawnX, h.spawnZ}
	if h.hasStronghold {
		sh := marker{h.strongholdX, h.strongholdZ}
		return spawn, &sh
	}
	return spawn, nil
}

// writeMapPNG renders a hit's map with an existing generator and writes it to
// disk. Used by the CLI.
func writeMapPNG(s unsafe.Pointer, h hit, step int, path string) error {
	spawn, sh := hitMarkers(h)
	img := renderImage(s, h.seed, step, spawn, sh)
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()
	return png.Encode(f, img)
}

// renderPNG renders a map straight to w. It owns its own generator, so it is
// safe to call concurrently. Used by the web server.
func renderPNG(w io.Writer, seed uint64, step int, spawn marker, sh *marker) error {
	s := C.scanner_new()
	defer C.scanner_free(s)
	img := renderImage(s, seed, step, spawn, sh)
	return png.Encode(w, img)
}
