package main

/*
#cgo CFLAGS: -I${SRCDIR}/cubiomes -O3
#cgo LDFLAGS: -L${SRCDIR}/cubiomes -lcubiomes -lm
#include <stdlib.h>
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

// mapSize is the width and height of the output image in pixels.
// mapStep is how many blocks each pixel covers, so the picture spans
// mapSize*mapStep blocks -- 512*4 = 2048 blocks across, centred on 0,0.
const (
	mapSize = 512
	mapStep = 4
	mapY    = 60
)

type hit struct {
	seed      uint64
	spawnX    int
	spawnZ    int
	lushCount int
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

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	// The renderer needs its own generator, for the same reason the workers do
	// -- see the comment in search().
	renderer := C.scanner_new()
	defer C.scanner_free(renderer)

	var tested int64
	found := 0
	runScan(ctx, *workers, &tested, func(h hit) bool {
		found++
		fmt.Printf("seed %d  spawn %d,%d  lush samples %d\n",
			int64(h.seed), h.spawnX, h.spawnZ, h.lushCount)

		path := filepath.Join(*outDir, fmt.Sprintf("%d.png", int64(h.seed)))
		if err := writeMapPNG(renderer, h, path); err != nil {
			log.Printf("render %d: %v", int64(h.seed), err)
		}

		// Returning false tells runScan to stop the workers.
		return found < *wanted
	})

	fmt.Printf("\ntested %d seeds\n", atomic.LoadInt64(&tested))
}

// runScan spins up the worker pool and calls onHit for every matching seed, in
// the order they arrive. onHit returns false to stop the scan; runScan also
// stops when ctx is cancelled (e.g. an HTTP client disconnecting). It blocks
// until every worker has exited, leaving no goroutines behind.
func runScan(parent context.Context, workers int, tested *int64, onHit func(hit) bool) {
	if workers < 1 {
		workers = 1
	}

	ctx, cancel := context.WithCancel(parent)
	defer cancel()

	hits := make(chan hit)
	var wg sync.WaitGroup
	for i := 0; i < workers; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			search(ctx, hits, tested)
		}()
	}

	// Close hits once every worker has returned so the range below terminates.
	go func() {
		wg.Wait()
		close(hits)
	}()

	stopped := false
	for h := range hits {
		if stopped {
			continue // draining so workers never block on a send
		}
		if !onHit(h) {
			stopped = true
			cancel() // workers select on ctx.Done() and return
		}
	}
}

func search(ctx context.Context, hits chan<- hit, tested *int64) {
	// One generator per goroutine. cubiomes' Generator struct holds mutable
	// state that applySeed() overwrites, so two goroutines sharing one would
	// silently corrupt each other's results -- no crash, just wrong answers.
	s := C.scanner_new()
	defer C.scanner_free(s)

	// Each worker gets its own random source. Sharing one would mean the
	// workers contend on a lock for every seed.
	rng := rand.NewPCG(rand.Uint64(), rand.Uint64())
	r := rand.New(rng)

	for {
		select {
		case <-ctx.Done():
			return
		default:
		}

		seed := r.Uint64()
		res := C.scanner_check(s, C.uint64_t(seed))
		atomic.AddInt64(tested, 1)

		if res.match == 0 {
			continue
		}
		select {
		case hits <- hit{
			seed:      seed,
			spawnX:    int(res.spawnX),
			spawnZ:    int(res.spawnZ),
			lushCount: int(res.lushCount),
		}:
		case <-ctx.Done():
			return
		}
	}
}

// palette pulls cubiomes' own biome colours across once, so the maps look like
// the ones you have seen from other seed tools. Loaded lazily and cached, since
// both the CLI and the web server want it.
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

// renderImage draws the biome map for a seed and marks spawn with a white
// cross. The generator s is borrowed for the duration of the call.
func renderImage(s unsafe.Pointer, seed uint64, spawnX, spawnZ int) *image.RGBA {
	// Allocate the grid in Go and hand C a pointer to it. Because the slice is
	// only borrowed for the duration of the call and C keeps no reference to
	// it, this is safe under cgo's pointer rules.
	grid := make([]C.int, mapSize*mapSize)
	C.scanner_biome_grid(s, C.uint64_t(seed), mapSize, mapStep, mapY,
		(*C.int)(unsafe.Pointer(&grid[0])))

	p := palette()
	img := image.NewRGBA(image.Rect(0, 0, mapSize, mapSize))
	for j := 0; j < mapSize; j++ {
		for i := 0; i < mapSize; i++ {
			id := int(grid[j*mapSize+i])
			if id < 0 || id > 255 {
				id = 0
			}
			img.SetRGBA(i, j, p[id])
		}
	}

	// Mark spawn with a bold white cross wrapped in a dark halo, so it stays
	// legible whether it lands on green, blue or sandy terrain. SetRGBA already
	// ignores out-of-bounds points, so no edge guard is needed.
	half := mapSize / 2
	sx := half + spawnX/mapStep
	sz := half + spawnZ/mapStep
	const arm = 7
	black := color.RGBA{0, 0, 0, 255}
	white := color.RGBA{255, 255, 255, 255}
	// Halo first: a fatter black cross underneath.
	for d := -arm - 1; d <= arm+1; d++ {
		for t := -2; t <= 2; t++ {
			img.SetRGBA(sx+d, sz+t, black)
			img.SetRGBA(sx+t, sz+d, black)
		}
	}
	// White cross on top, 3px thick.
	for d := -arm; d <= arm; d++ {
		for t := -1; t <= 1; t++ {
			img.SetRGBA(sx+d, sz+t, white)
			img.SetRGBA(sx+t, sz+d, white)
		}
	}
	return img
}

// writeMapPNG renders a hit's map using an existing generator and writes it to
// disk. Used by the CLI.
func writeMapPNG(s unsafe.Pointer, h hit, path string) error {
	img := renderImage(s, h.seed, h.spawnX, h.spawnZ)
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()
	return png.Encode(f, img)
}

// renderPNG renders a map straight to w (e.g. an HTTP response). It owns its own
// generator, so it is safe to call concurrently. Used by the web server.
func renderPNG(w io.Writer, seed uint64, spawnX, spawnZ int) error {
	s := C.scanner_new()
	defer C.scanner_free(s)
	img := renderImage(s, seed, spawnX, spawnZ)
	return png.Encode(w, img)
}
