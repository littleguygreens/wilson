package main

import (
	_ "embed"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"runtime"
	"strconv"
	"sync"
	"sync/atomic"
	"time"
)

//go:embed web/index.html
var indexHTML []byte

// maxWorkers caps what a single web request can ask for, so a stray URL can't
// spin up thousands of goroutines.
const maxWorkers = 64

func runServer(addr string) error {
	// Warm the palette once up front rather than on the first map request.
	palette()

	mux := http.NewServeMux()
	mux.HandleFunc("/", indexHandler)
	mux.HandleFunc("/api/scan", scanHandler)
	mux.HandleFunc("/api/map", mapHandler)

	log.Printf("seedscan web UI on http://localhost%s  (open it from your phone using this machine's LAN IP)", addr)
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

// matchDTO is the JSON shape sent to the browser for each matching seed. Seed
// is signed to match how Minecraft and the CLI display Java seeds.
type matchDTO struct {
	Seed   int64 `json:"seed"`
	SpawnX int   `json:"spawnX"`
	SpawnZ int   `json:"spawnZ"`
	Lush   int   `json:"lush"`
}

// scanHandler streams scan results to the browser as Server-Sent Events. The
// scan is tied to the request context, so when the browser closes the
// EventSource the workers stop.
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

	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("Connection", "keep-alive")

	sw := &sseWriter{w: w, f: flusher}

	var tested int64

	// A background ticker reports how many seeds have been checked, so the page
	// shows progress even during long dry spells between matches.
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
	runScan(ctx, workers, &tested, func(h hit) bool {
		found++
		sw.send("match", matchDTO{
			Seed:   int64(h.seed),
			SpawnX: h.spawnX,
			SpawnZ: h.spawnZ,
			Lush:   h.lushCount,
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

// mapHandler renders the biome map PNG for one seed. Spawn coordinates come from
// the match event so we don't have to recompute the (expensive) spawn point.
func mapHandler(w http.ResponseWriter, r *http.Request) {
	seed, err := strconv.ParseInt(r.URL.Query().Get("seed"), 10, 64)
	if err != nil {
		http.Error(w, "bad seed", http.StatusBadRequest)
		return
	}
	sx := queryInt(r, "sx", 0)
	sz := queryInt(r, "sz", 0)

	w.Header().Set("Content-Type", "image/png")
	w.Header().Set("Cache-Control", "public, max-age=3600")
	if err := renderPNG(w, uint64(seed), sx, sz); err != nil {
		log.Printf("render map for seed %d: %v", seed, err)
	}
}

// sseWriter serialises writes to the response, since the match/progress/done
// events can be produced from different goroutines.
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
