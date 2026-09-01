package main

/*
#cgo CFLAGS: -I${SRCDIR}/cubiomes -O3
#cgo LDFLAGS: -L${SRCDIR}/cubiomes -lcubiomes -lm
#include <stdlib.h>
#include "generator.h"
#include "finders.h"
#include "util.h"
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
	"math"
	"math/rand/v2"
	"os"
	"path/filepath"
	"runtime"
	"strconv"
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
	Surface     []int32
	SurfaceMode []int32
	Ocean       []int32
	OceanMode   []int32
	Cave        []int32
	CaveMode    []int32
	MinCave     int

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

	Structures   []int32
	StructMode   []int32
	StructRadius int

	MapStep int  // rendering zoom; not part of the C filter
	Exp263  bool // experimental Minecraft 26.3 (Dappled Forest) relabel
}

// biomeEntry ties a stable UI key to a cubiomes biome id. The id stays
// unexported so the C dependency doesn't leak out of this file.
type biomeEntry struct {
	Category string
	Key      string
	Label    string
	id       int32
	exp263   bool // only offered in the experimental 26.3 mode
}

// dappledForestID is the biome id for Dappled Forest. cubiomes' enum stops at
// pale_garden(186); 26.3's Dappled Forest is synthesised by scan.c's relabel, so
// it lives just past the enum. Keep it in sync with B_DAPPLED_FOREST in scan.c.
const dappledForestID int32 = 187

// sulfurCavesID is the 26.3 Sulfur Caves biome id (synthesised by scan.c's
// gated relabel). Keep in sync with B_SULFUR_CAVES in entries263.h.
const sulfurCavesID int32 = 188

// catalog is the authoritative list of selectable biomes. The web menu is built
// from it (keys + labels), and search requests map keys back to ids through it.
var catalog = []biomeEntry{
	// Island surface biomes.
	{"surface", "plains", "Plains", int32(C.plains), false},
	{"surface", "snowy_plains", "Snowy Plains", int32(C.snowy_plains), false},
	{"surface", "forest", "Forest", int32(C.forest), false},
	{"surface", "flower_forest", "Flower Forest", int32(C.flower_forest), false},
	{"surface", "birch_forest", "Birch Forest", int32(C.birch_forest), false},
	{"surface", "dark_forest", "Dark Forest", int32(C.dark_forest), false},
	{"surface", "taiga", "Taiga", int32(C.taiga), false},
	{"surface", "snowy_taiga", "Snowy Taiga", int32(C.snowy_taiga), false},
	{"surface", "old_growth_pine_taiga", "Old Growth Pine Taiga", int32(C.old_growth_pine_taiga), false},
	{"surface", "jungle", "Jungle", int32(C.jungle), false},
	{"surface", "sparse_jungle", "Sparse Jungle", int32(C.sparse_jungle), false},
	{"surface", "bamboo_jungle", "Bamboo Jungle", int32(C.bamboo_jungle), false},
	{"surface", "savanna", "Savanna", int32(C.savanna), false},
	{"surface", "desert", "Desert", int32(C.desert), false},
	{"surface", "swamp", "Swamp", int32(C.swamp), false},
	{"surface", "mangrove_swamp", "Mangrove Swamp", int32(C.mangrove_swamp), false},
	{"surface", "badlands", "Badlands", int32(C.badlands), false},
	{"surface", "mushroom_fields", "Mushroom Fields", int32(C.mushroom_fields), false},
	{"surface", "cherry_grove", "Cherry Grove", int32(C.cherry_grove), false},
	{"surface", "pale_garden", "Pale Garden", int32(C.pale_garden), false},
	{"surface", "dappled_forest", "Dappled Forest", dappledForestID, true},
	{"surface", "meadow", "Meadow", int32(C.meadow), false},
	{"surface", "grove", "Grove", int32(C.grove), false},
	{"surface", "snowy_slopes", "Snowy Slopes", int32(C.snowy_slopes), false},
	{"surface", "windswept_hills", "Windswept Hills", int32(C.windswept_hills), false},
	{"surface", "windswept_forest", "Windswept Forest", int32(C.windswept_forest), false},
	{"surface", "jagged_peaks", "Jagged Peaks", int32(C.jagged_peaks), false},
	{"surface", "frozen_peaks", "Frozen Peaks", int32(C.frozen_peaks), false},
	{"surface", "stony_peaks", "Stony Peaks", int32(C.stony_peaks), false},
	{"surface", "stony_shore", "Stony Shore", int32(C.stony_shore), false},

	// Allowed ocean types for the isolation rings.
	{"ocean", "ocean", "Ocean", int32(C.ocean), false},
	{"ocean", "deep_ocean", "Deep Ocean", int32(C.deep_ocean), false},
	{"ocean", "warm_ocean", "Warm Ocean", int32(C.warm_ocean), false},
	{"ocean", "lukewarm_ocean", "Lukewarm Ocean", int32(C.lukewarm_ocean), false},
	{"ocean", "deep_lukewarm_ocean", "Deep Lukewarm", int32(C.deep_lukewarm_ocean), false},
	{"ocean", "cold_ocean", "Cold Ocean", int32(C.cold_ocean), false},
	{"ocean", "deep_cold_ocean", "Deep Cold", int32(C.deep_cold_ocean), false},
	{"ocean", "frozen_ocean", "Frozen Ocean", int32(C.frozen_ocean), false},
	{"ocean", "deep_frozen_ocean", "Deep Frozen", int32(C.deep_frozen_ocean), false},

	// Underground cave biomes.
	{"cave", "lush_caves", "Lush Caves", int32(C.lush_caves), false},
	{"cave", "dripstone_caves", "Dripstone Caves", int32(C.dripstone_caves), false},
	{"cave", "deep_dark", "Deep Dark", int32(C.deep_dark), false},
	{"cave", "sulfur_caves", "Sulfur Caves", sulfurCavesID, true},
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

// structEntry ties a UI key to a cubiomes StructureType, a label and a map
// marker colour. Mineshafts are omitted: they use a different cubiomes API and
// are near-ubiquitous underground, so they make a poor filter and a noisy marker.
type structEntry struct {
	Key    string
	Label  string
	Color  string
	id     int32
	exp263 bool // only offered in the experimental 26.3 mode
}

var structCatalog = []structEntry{
	{"village", "Village", "#f2c14e", int32(C.Village), false},
	{"outpost", "Pillager Outpost", "#e05a4f", int32(C.Outpost), false},
	{"mansion", "Woodland Mansion", "#b07a3f", int32(C.Mansion), false},
	{"monument", "Ocean Monument", "#37b7a8", int32(C.Monument), false},
	{"ruined_portal", "Ruined Portal", "#a878f0", int32(C.Ruined_Portal), false},
	{"ancient_city", "Ancient City", "#4a90d9", int32(C.Ancient_City), false},
	{"trial_chambers", "Trial Chamber", "#e8873a", int32(C.Trial_Chambers), false},
	{"abandoned_camp", "Abandoned Camp", "#8fbf6f", int32(C.STRUCT_ABANDONED_CAMP), true},
}

func structKeyToID(key string) (int32, bool) {
	for _, e := range structCatalog {
		if e.Key == key {
			return e.id, true
		}
	}
	return 0, false
}

// structuresAt returns the (x,z) positions of one structure type within a
// square of half-size `half` centred on the origin, for a seed.
func structuresAt(seed uint64, structType int32, half int, exp263 bool) [][2]int {
	s := biomeGenPool.Get().(unsafe.Pointer)
	defer biomeGenPool.Put(s)
	const max = 256
	out := make([]C.int, max*2)
	n := int(C.scanner_structures(s, C.uint64_t(seed), C.int(structType), boolToC(exp263),
		C.int(-half), C.int(-half), C.int(half), C.int(half),
		(*C.int)(unsafe.Pointer(&out[0])), C.int(max)))
	if n > max {
		n = max
	}
	res := make([][2]int, 0, n)
	for i := 0; i < n; i++ {
		res = append(res, [2]int{int(out[i*2]), int(out[i*2+1])})
	}
	return res
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
	putList(&c.surfaceMode, cfg.SurfaceMode)
	c.nOcean = putList(&c.ocean, cfg.Ocean)
	putList(&c.oceanMode, cfg.OceanMode)
	c.nCave = putList(&c.cave, cfg.Cave)
	putList(&c.caveMode, cfg.CaveMode)
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

	c.nStructures = putList(&c.structures, cfg.Structures)
	putList(&c.structMode, cfg.StructMode)
	c.structRadius = C.int(cfg.StructRadius)
	c.exp263 = boolToC(cfg.Exp263)
	return c
}

// Selection modes, mirrored from scan.h (SEL_INCLUDED/REQUIRED/EXCLUDED).
const (
	selIncluded int32 = 0
	selRequired int32 = 1
	selExcluded int32 = 2
)

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
	check := flag.String("check", "", "diagnose a single seed against -size, then exit")
	size := flag.String("size", "L", "size preset for -check (S/M/L/XL/XXL)")
	moat := flag.Int("moat", 0, "min mainland moat (blocks) for -check")
	oceanInc := flag.String("oceanInc", "", "comma-separated included ocean types for -check (whitelist)")
	oceanReq := flag.String("oceanReq", "", "comma-separated required ocean types for -check")
	oceanExc := flag.String("oceanExc", "", "comma-separated excluded ocean types for -check")
	flag.Parse()

	if *check != "" {
		runCheck(*check, *size, *moat, *oceanReq, *oceanInc, *oceanExc)
		return
	}

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
	cfg.SurfaceMode = repeatMode(selIncluded, len(peaks)) // any peak
	cfg.Cave = []int32{lush}
	cfg.CaveMode = []int32{selRequired} // require lush caves
	cfg.MinCave = 3
	cfg.RequireEnclosed = true
	return cfg
}

func repeatMode(mode int32, n int) []int32 {
	out := make([]int32, n)
	for i := range out {
		out[i] = mode
	}
	return out
}

// runCheck diagnoses one seed against a size preset (enclosure on) and prints
// whether it would match. Handy for confirming a build behaves as expected:
//
//	./wilson -check <seed> -size L [-moat 300]
func runCheck(seedStr, size string, moat int, oceanReq, oceanInc, oceanExc string) {
	seed, err := strconv.ParseInt(seedStr, 10, 64)
	if err != nil {
		log.Fatalf("bad seed %q: %v", seedStr, err)
	}
	cfg := geometryForSize(size)
	cfg.RequireEnclosed = true
	cfg.MinMoat = moat
	cfg.Ocean, cfg.OceanMode = triSelection(oceanReq, oceanInc, oceanExc, biomeKeyToID)
	if moat > 0 { // mirror the server's window growth
		if needed := cfg.IslandRadius + moat + 4*cfg.IslandStep; needed > cfg.IslandWindow {
			cfg.IslandWindow = needed
		}
		if cfg.IslandWindow > maxIslandWindow {
			cfg.IslandWindow = maxIslandWindow
		}
	}
	cc := toCConfig(cfg)
	s := C.scanner_new()
	defer C.scanner_free(s)
	r := C.scanner_check(s, C.uint64_t(uint64(seed)), &cc)

	fmt.Printf("seed %d  size %s  enclosed  moat>=%d\n", seed, size, moat)
	if r.match == 0 {
		fmt.Println("  match: NO  — rejected (not an enclosed island under this size)")
		return
	}
	step := cfg.IslandStep
	width := int(math.Round(2 * math.Sqrt(float64(int(r.islandCells)*step*step)/math.Pi)))
	moatStr := "no mainland within window"
	if int(r.moatBlocks) >= 0 {
		moatStr = fmt.Sprintf("%d blocks to mainland", int(r.moatBlocks))
	}
	fmt.Printf("  match: YES  spawn %d,%d  island ~%d blocks wide  %s\n",
		int(r.spawnX), int(r.spawnZ), width, moatStr)
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

// inspectSeed reports one seed's spawn and island metrics without filtering, for
// the "view a seed" feature. cfg supplies the size geometry (window/step/zoom).
func inspectSeed(seed uint64, cfg Config) hit {
	s := C.scanner_new()
	defer C.scanner_free(s)
	cc := toCConfig(cfg)
	r := C.scanner_inspect(s, C.uint64_t(seed), &cc)
	return hit{
		seed:        seed,
		spawnX:      int(r.spawnX),
		spawnZ:      int(r.spawnZ),
		islandCells: int(r.islandCells),
		moatBlocks:  int(r.moatBlocks),
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
		// Dappled Forest (187) and Sulfur Caves (188) have no cubiomes colour;
		// give them distinct hues for the maps. Dappled Forest is a bright red so
		// it can't be mistaken for the grey-tan of stony shore at a glance.
		paletteData[dappledForestID] = color.RGBA{R: 230, G: 57, B: 53, A: 255}
		paletteData[sulfurCavesID] = color.RGBA{R: 227, G: 206, B: 58, A: 255}
	})
	return paletteData
}

// renderImage draws the biome map for a seed at the given block-per-pixel step
// and sample height y, marks spawn, and (if present) marks the stronghold.
func renderImage(s unsafe.Pointer, seed uint64, step, y int, exp263 bool, spawn marker, sh *marker) *image.RGBA {
	if step < 1 {
		step = 4
	}
	grid := make([]C.int, mapPixels*mapPixels)
	C.scanner_biome_grid(s, C.uint64_t(seed), mapPixels, C.int(step), C.int(y),
		boolToC(exp263), (*C.int)(unsafe.Pointer(&grid[0])))

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
	img := renderImage(s, h.seed, step, 60, false, spawn, sh)
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()
	return png.Encode(f, img)
}

// renderPNG renders a map straight to w at sample height y. It owns its own
// generator, so it is safe to call concurrently. Used by the web server.
func renderPNG(w io.Writer, seed uint64, step, y int, exp263 bool, spawn marker, sh *marker) error {
	s := C.scanner_new()
	defer C.scanner_free(s)
	img := renderImage(s, seed, step, y, exp263, spawn, sh)
	return png.Encode(w, img)
}

// caveOverlayColor gives a bold outline colour for each cave biome (transparent
// otherwise). Must match the legend swatches in web/index.html.
func caveOverlayColor(id int) (color.RGBA, bool) {
	switch id {
	case int(C.lush_caves):
		return color.RGBA{0x55, 0xd1, 0x6a, 0xff}, true
	case int(C.dripstone_caves):
		return color.RGBA{0xd1, 0x8a, 0x3a, 0xff}, true
	case int(C.deep_dark):
		return color.RGBA{0x35, 0xc9, 0xd6, 0xff}, true
	case int(sulfurCavesID):
		return color.RGBA{0xe3, 0xce, 0x3a, 0xff}, true
	}
	return color.RGBA{}, false
}

// renderCaveOverlay builds a transparent image containing only the outlines of
// cave biomes at height y, to stack over the surface map so you can see what
// cave sits under what surface biome.
func renderCaveOverlay(s unsafe.Pointer, seed uint64, step, y int, exp263 bool) *image.RGBA {
	if step < 1 {
		step = 4
	}
	grid := make([]C.int, mapPixels*mapPixels)
	C.scanner_biome_grid(s, C.uint64_t(seed), mapPixels, C.int(step), C.int(y),
		boolToC(exp263), (*C.int)(unsafe.Pointer(&grid[0])))
	img := image.NewRGBA(image.Rect(0, 0, mapPixels, mapPixels))
	at := func(i, j int) int { return int(grid[j*mapPixels+i]) }
	for j := 0; j < mapPixels; j++ {
		for i := 0; i < mapPixels; i++ {
			id := at(i, j)
			col, ok := caveOverlayColor(id)
			if !ok {
				continue
			}
			// A cave cell is an outline pixel if any 4-neighbour is a different
			// biome (or the grid edge) -- i.e. it sits on the region border.
			edge := i == 0 || j == 0 || i == mapPixels-1 || j == mapPixels-1 ||
				at(i-1, j) != id || at(i+1, j) != id || at(i, j-1) != id || at(i, j+1) != id
			if !edge {
				continue
			}
			img.SetRGBA(i, j, col) // 2px line for legibility over the surface map
			if i+1 < mapPixels {
				img.SetRGBA(i+1, j, col)
			}
			if j+1 < mapPixels {
				img.SetRGBA(i, j+1, col)
			}
		}
	}
	return img
}

func renderCaveOverlayPNG(w io.Writer, seed uint64, step, y int, exp263 bool) error {
	s := C.scanner_new()
	defer C.scanner_free(s)
	img := renderCaveOverlay(s, seed, step, y, exp263)
	return png.Encode(w, img)
}

// biomeGenPool reuses generators across biome lookups so each hover request
// doesn't rebuild one from scratch. Pooled generators live for the process.
var biomeGenPool = sync.Pool{New: func() any { return C.scanner_new() }}

// biomeAt returns the biome id and name at a block coordinate for one seed.
func biomeAt(seed uint64, x, y, z int, exp263 bool) (int, string) {
	s := biomeGenPool.Get().(unsafe.Pointer)
	defer biomeGenPool.Put(s)
	id := int(C.scanner_biome(s, C.uint64_t(seed), C.int(x), C.int(y), C.int(z), boolToC(exp263)))
	var name string
	switch int32(id) {
	case dappledForestID:
		name = "dappled_forest" // synthesised ids; cubiomes' biome2str can't name them
	case sulfurCavesID:
		name = "sulfur_caves"
	default:
		name = C.GoString(C.biome2str(C.MC_1_21, C.int(id)))
	}
	if name == "" {
		name = fmt.Sprintf("biome %d", id)
	}
	return id, name
}
