package main

import (
	_ "embed"
	"encoding/json"
	"fmt"
	"log"
	"math"
	"net/http"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

//go:embed web/index.html
var indexHTML []byte

const (
	maxWorkers      = 64
	maxSurface      = 6 // how many island biomes a search may request
	defaultMinCav   = 3
	maxIslandWindow = 6000 // cap on the flood-fill half-window (blocks)
)

// sizePreset maps an island-size choice to search geometry and map zoom. Bigger
// sizes push the ocean rings out and zoom the map so the whole landmass fits.
// XXL ("continent") drops the inner ring (spawn is deep inland) and leans on the
// far outer ring plus a high land floor.
type sizePreset struct {
	Key            string `json:"key"`
	Label          string `json:"label"`
	IslandRadius   int    `json:"-"`
	GridStep       int    `json:"-"`
	MinLandPercent int    `json:"-"`
	RingRadius     int    `json:"-"`
	RingSamples    int    `json:"-"`
	RingMinOcean   int    `json:"-"`
	OuterRadius    int    `json:"-"`
	OuterSamples   int    `json:"-"`
	OuterMinOcean  int    `json:"-"`
	IslandWindow   int    `json:"-"`
	IslandStep     int    `json:"-"`
	MinIslandCells int    `json:"-"`
	MapStep        int    `json:"-"`
}

var sizePresets = []sizePreset{
	{Key: "S", Label: "Small", IslandRadius: 120, GridStep: 12, MinLandPercent: 20,
		RingRadius: 160, RingSamples: 32, RingMinOcean: 28,
		OuterRadius: 380, OuterSamples: 40, OuterMinOcean: 30,
		IslandWindow: 500, IslandStep: 4, MinIslandCells: 30, MapStep: 2},
	{Key: "M", Label: "Medium", IslandRadius: 200, GridStep: 16, MinLandPercent: 25,
		RingRadius: 250, RingSamples: 32, RingMinOcean: 30,
		OuterRadius: 600, OuterSamples: 48, OuterMinOcean: 38,
		IslandWindow: 800, IslandStep: 4, MinIslandCells: 60, MapStep: 4},
	{Key: "L", Label: "Large", IslandRadius: 320, GridStep: 20, MinLandPercent: 35,
		RingRadius: 420, RingSamples: 40, RingMinOcean: 34,
		OuterRadius: 900, OuterSamples: 48, OuterMinOcean: 36,
		IslandWindow: 1300, IslandStep: 8, MinIslandCells: 60, MapStep: 6},
	{Key: "XL", Label: "Huge", IslandRadius: 520, GridStep: 28, MinLandPercent: 45,
		RingRadius: 660, RingSamples: 48, RingMinOcean: 38,
		OuterRadius: 1300, OuterSamples: 48, OuterMinOcean: 34,
		IslandWindow: 2000, IslandStep: 8, MinIslandCells: 120, MapStep: 10},
	{Key: "XXL", Label: "Continent", IslandRadius: 1400, GridStep: 64, MinLandPercent: 70,
		RingRadius: 0, RingSamples: 0, RingMinOcean: 0,
		OuterRadius: 1800, OuterSamples: 48, OuterMinOcean: 34,
		IslandWindow: 3200, IslandStep: 16, MinIslandCells: 200, MapStep: 16},
}

func presetForSize(key string) sizePreset {
	for _, p := range sizePresets {
		if p.Key == key {
			return p
		}
	}
	return sizePresets[1] // default Medium
}

// geometryForSize returns a Config with only the geometry/zoom fields filled.
func geometryForSize(key string) Config {
	p := presetForSize(key)
	return Config{
		IslandRadius:   p.IslandRadius,
		GridStep:       p.GridStep,
		MinLandPercent: p.MinLandPercent,
		RingRadius:     p.RingRadius,
		RingSamples:    p.RingSamples,
		RingMinOcean:   p.RingMinOcean,
		OuterRadius:    p.OuterRadius,
		OuterSamples:   p.OuterSamples,
		OuterMinOcean:  p.OuterMinOcean,
		IslandWindow:   p.IslandWindow,
		IslandStep:     p.IslandStep,
		MinIslandCells: p.MinIslandCells,
		MapStep:        p.MapStep,
	}
}

func runServer(addr string) error {
	palette() // warm the biome colours once

	mux := http.NewServeMux()
	mux.HandleFunc("/", indexHandler)
	mux.HandleFunc("/api/biomes", biomesHandler)
	mux.HandleFunc("/api/scan", scanHandler)
	mux.HandleFunc("/api/map", mapHandler)
	mux.HandleFunc("/api/biome", biomeHandler)
	mux.HandleFunc("/api/structures", structuresHandler)

	log.Printf("wilson web UI on http://localhost%s  (open it from your phone using this machine's LAN IP)", addr)
	return http.ListenAndServe(addr, mux)
}

func indexHandler(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Write(indexHTML)
}

// biomesHandler feeds the menu: selectable biomes by category, and the sizes.
func biomesHandler(w http.ResponseWriter, r *http.Request) {
	type opt struct {
		Key   string `json:"key"`
		Label string `json:"label"`
	}
	type structOpt struct {
		Key   string `json:"key"`
		Label string `json:"label"`
		Color string `json:"color"`
	}
	out := struct {
		Surface    []opt        `json:"surface"`
		Ocean      []opt        `json:"ocean"`
		Cave       []opt        `json:"cave"`
		Structures []structOpt  `json:"structures"`
		Sizes      []sizePreset `json:"sizes"`
		MaxSurface int          `json:"maxSurface"`
	}{Sizes: sizePresets, MaxSurface: maxSurface}

	for _, e := range catalog {
		o := opt{Key: e.Key, Label: e.Label}
		switch e.Category {
		case "surface":
			out.Surface = append(out.Surface, o)
		case "ocean":
			out.Ocean = append(out.Ocean, o)
		case "cave":
			out.Cave = append(out.Cave, o)
		}
	}
	for _, e := range structCatalog {
		out.Structures = append(out.Structures, structOpt{Key: e.Key, Label: e.Label, Color: e.Color})
	}
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(out)
}

// structuresHandler returns the positions of the requested structure types
// within a square of half-size `half` around the origin, for the map overlays.
func structuresHandler(w http.ResponseWriter, r *http.Request) {
	seed, err := strconv.ParseInt(r.URL.Query().Get("seed"), 10, 64)
	if err != nil {
		http.Error(w, "bad seed", http.StatusBadRequest)
		return
	}
	half := queryInt(r, "half", 1536)
	if half < 1 {
		half = 1
	}
	if half > 100000 {
		half = 100000
	}
	out := map[string][][2]int{}
	for _, k := range strings.Split(r.URL.Query().Get("types"), ",") {
		k = strings.TrimSpace(k)
		if k == "" {
			continue
		}
		if id, ok := structKeyToID(k); ok {
			out[k] = structuresAt(uint64(seed), id, half)
		}
	}
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(out)
}

// keysToIDs maps comma-separated UI keys to biome ids, dropping unknown ones and
// capping the count when limit > 0.
func keysToIDs(csv string, limit int) []int32 {
	var ids []int32
	for _, k := range strings.Split(csv, ",") {
		k = strings.TrimSpace(k)
		if k == "" {
			continue
		}
		if id, ok := biomeKeyToID(k); ok {
			ids = append(ids, id)
			if limit > 0 && len(ids) >= limit {
				break
			}
		}
	}
	return ids
}

const scanMaxList = 16 // mirror of SCAN_MAX_LIST in scan.h

// triSelection builds parallel id + mode lists from required/included/excluded
// comma-separated keys (required first). resolve maps a key to an id.
func triSelection(req, inc, exc string, resolve func(string) (int32, bool)) (ids, modes []int32) {
	add := func(csv string, mode int32) {
		for _, k := range strings.Split(csv, ",") {
			k = strings.TrimSpace(k)
			if k == "" {
				continue
			}
			if id, ok := resolve(k); ok && len(ids) < scanMaxList {
				ids = append(ids, id)
				modes = append(modes, mode)
			}
		}
	}
	add(req, selRequired)
	add(inc, selIncluded)
	add(exc, selExcluded)
	return
}

// configFromQuery builds a search Config from the request parameters.
func configFromQuery(r *http.Request) Config {
	q := r.URL.Query()
	cfg := geometryForSize(q.Get("size"))

	cfg.Surface, cfg.SurfaceMode = triSelection(q.Get("surfaceReq"), q.Get("surfaceInc"), q.Get("surfaceExc"), biomeKeyToID)
	cfg.Ocean = keysToIDs(q.Get("ocean"), 0)
	cfg.Cave, cfg.CaveMode = triSelection(q.Get("caveReq"), q.Get("caveInc"), q.Get("caveExc"), biomeKeyToID)
	cfg.MinCave = queryInt(r, "minCave", defaultMinCav)
	if cfg.MinCave < 1 {
		cfg.MinCave = 1
	}

	if q.Get("stronghold") == "1" {
		cfg.RequireStronghold = true
		cfg.StrongholdMaxDist = queryInt(r, "shDist", 1700)
	}

	if sid, smode := triSelection(q.Get("structReq"), q.Get("structInc"), q.Get("structExc"), structKeyToID); len(sid) > 0 {
		cfg.Structures = sid
		cfg.StructMode = smode
		cfg.StructRadius = cfg.IslandRadius
	}

	// Full-enclosure flood fill is always on: results are never peninsulas.
	cfg.RequireEnclosed = true
	cfg.MinMoat = queryInt(r, "moat", 0)
	if cfg.MinMoat < 0 {
		cfg.MinMoat = 0
	}
	// Grow the flood-fill window so the requested moat actually fits inside it,
	// capped so the grid can't blow up.
	if cfg.RequireEnclosed && cfg.MinMoat > 0 {
		if needed := cfg.IslandRadius + cfg.MinMoat + 4*cfg.IslandStep; needed > cfg.IslandWindow {
			cfg.IslandWindow = needed
		}
		if cfg.IslandWindow > maxIslandWindow {
			cfg.IslandWindow = maxIslandWindow
		}
	}
	return cfg
}

// matchDTO is the JSON shape sent to the browser for each matching seed. Seed is
// a string, not a number: JSON numbers become 64-bit floats in the browser,
// which lose the low bits of a full 64-bit Minecraft seed and would point at a
// different (usually non-island) seed.
type matchDTO struct {
	Seed           string `json:"seed"`
	SpawnX         int    `json:"spawnX"`
	SpawnZ         int    `json:"spawnZ"`
	Caves          int    `json:"caves"`
	Step           int    `json:"step"`
	HasStronghold  bool   `json:"hasStronghold"`
	StrongholdX    int    `json:"strongholdX"`
	StrongholdZ    int    `json:"strongholdZ"`
	StrongholdDist int    `json:"strongholdDist"`
	IslandBlocks   int64  `json:"islandBlocks"`
	Enclosed       bool   `json:"enclosed"`
	Moat           int    `json:"moat"` // sea gap to nearest other land; -1 = open
}

// scanHandler streams scan results as Server-Sent Events, tied to the request
// context so closing the browser stops the workers.
func scanHandler(w http.ResponseWriter, r *http.Request) {
	flusher, ok := w.(http.Flusher)
	if !ok {
		http.Error(w, "streaming unsupported", http.StatusInternalServerError)
		return
	}

	n := queryInt(r, "n", 5)
	if n < 1 {
		n = 1
	}
	workers := queryInt(r, "workers", runtime.NumCPU())
	if workers < 1 {
		workers = runtime.NumCPU()
	}
	if workers > maxWorkers {
		workers = maxWorkers
	}

	cfg := configFromQuery(r)

	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("Connection", "keep-alive")

	sw := &sseWriter{w: w, f: flusher}
	var tested int64

	ctx := r.Context()
	stopTicker := make(chan struct{})
	var tickerDone sync.WaitGroup
	tickerDone.Add(1)
	go func() {
		defer tickerDone.Done()
		t := time.NewTicker(500 * time.Millisecond)
		defer t.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-stopTicker:
				return
			case <-t.C:
				sw.send("progress", map[string]int64{"tested": atomic.LoadInt64(&tested)})
			}
		}
	}()

	found := 0
	runScan(ctx, workers, &tested, cfg, func(h hit) bool {
		found++
		dist := int(math.Round(math.Hypot(float64(h.strongholdX), float64(h.strongholdZ))))
		sw.send("match", matchDTO{
			Seed:           strconv.FormatInt(int64(h.seed), 10),
			SpawnX:         h.spawnX,
			SpawnZ:         h.spawnZ,
			Caves:          h.caveCount,
			Step:           cfg.MapStep,
			HasStronghold:  h.hasStronghold,
			StrongholdX:    h.strongholdX,
			StrongholdZ:    h.strongholdZ,
			StrongholdDist: dist,
			IslandBlocks:   int64(h.islandCells) * int64(cfg.IslandStep) * int64(cfg.IslandStep),
			Enclosed:       cfg.RequireEnclosed,
			Moat:           h.moatBlocks,
		})
		return found < n
	})

	close(stopTicker)
	tickerDone.Wait()
	sw.send("done", map[string]int64{
		"tested": atomic.LoadInt64(&tested),
		"found":  int64(found),
	})
}

// mapHandler renders the biome map PNG for one seed at the requested zoom, with
// spawn and (optionally) stronghold markers.
func mapHandler(w http.ResponseWriter, r *http.Request) {
	seed, err := strconv.ParseInt(r.URL.Query().Get("seed"), 10, 64)
	if err != nil {
		http.Error(w, "bad seed", http.StatusBadRequest)
		return
	}
	step := queryInt(r, "step", 4)
	y := queryInt(r, "y", 60)
	spawn := marker{queryInt(r, "sx", 0), queryInt(r, "sz", 0)}

	var sh *marker
	if r.URL.Query().Get("st") == "1" {
		sh = &marker{queryInt(r, "stx", 0), queryInt(r, "stz", 0)}
	}

	w.Header().Set("Content-Type", "image/png")
	w.Header().Set("Cache-Control", "public, max-age=3600")
	if err := renderPNG(w, uint64(seed), step, y, spawn, sh); err != nil {
		log.Printf("render map for seed %d: %v", seed, err)
	}
}

// biomeHandler returns the biome id and name at a block coordinate, for the
// map hover tooltip. y defaults to the surface height.
func biomeHandler(w http.ResponseWriter, r *http.Request) {
	seed, err := strconv.ParseInt(r.URL.Query().Get("seed"), 10, 64)
	if err != nil {
		http.Error(w, "bad seed", http.StatusBadRequest)
		return
	}
	x := queryInt(r, "x", 0)
	z := queryInt(r, "z", 0)
	y := queryInt(r, "y", 60)
	id, name := biomeAt(uint64(seed), x, y, z)
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]any{"id": id, "name": name})
}

// sseWriter serialises event writes across the ticker and scan goroutines.
type sseWriter struct {
	w  http.ResponseWriter
	f  http.Flusher
	mu sync.Mutex
}

func (s *sseWriter) send(event string, data any) {
	b, err := json.Marshal(data)
	if err != nil {
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	fmt.Fprintf(s.w, "event: %s\ndata: %s\n\n", event, b)
	s.f.Flush()
}

func queryInt(r *http.Request, key string, def int) int {
	v := r.URL.Query().Get(key)
	if v == "" {
		return def
	}
	n, err := strconv.Atoi(v)
	if err != nil {
		return def
	}
	return n
}
