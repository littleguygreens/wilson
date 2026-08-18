package main

/*
#cgo CFLAGS: -I${SRCDIR}/cubiomes -O3
#cgo LDFLAGS: -L${SRCDIR}/cubiomes -lcubiomes -lm
#include <stdlib.h>
#include "scan.h"
*/
import "C"

import (
	"flag"
	"fmt"
	"image"
	"image/color"
	"image/png"
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
	flag.Parse()

	if err := os.MkdirAll(*outDir, 0o755); err != nil {
		log.Fatal(err)
	}

	var tested, found int64
	hits := make(chan hit)
	stop := make(chan struct{})

	var wg sync.WaitGroup
	for i := 0; i < *workers; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			search(hits, stop, &tested)
		}()
	}

	// The renderer needs its own generator too, for the same reason the
	// workers do -- see the comment in search().
	renderer := C.scanner_new()
	defer C.scanner_free(renderer)
	palette := loadPalette()

	for h := range hits {
		n := atomic.AddInt64(&found, 1)
		fmt.Printf("seed %d  spawn %d,%d  lush samples %d\n",
			int64(h.seed), h.spawnX, h.spawnZ, h.lushCount)

		path := filepath.Join(*outDir, fmt.Sprintf("%d.png", int64(h.seed)))
		if err := renderMap(renderer, h, palette, path); err != nil {
			log.Printf("render %d: %v", int64(h.seed), err)
		}

		if n >= int64(*wanted) {
			close(stop)
			break
		}
	}

	// Drain so the workers can exit rather than block on an unread channel.
	go func() {
		for range hits {
		}
	}()
	wg.Wait()

	fmt.Printf("\ntested %d seeds\n", atomic.LoadInt64(&tested))
}

func search(hits chan<- hit, stop <-chan struct{}, tested *int64) {
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
		case <-stop:
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
		case <-stop:
			return
		}
	}
}

// loadPalette pulls cubiomes' own biome colours across once, so the maps look
// like the ones you have seen from other seed tools.
func loadPalette() [256]color.RGBA {
	var raw [768]C.uchar
	C.scanner_colors((*C.uchar)(unsafe.Pointer(&raw[0])))

	var p [256]color.RGBA
	for i := 0; i < 256; i++ {
		p[i] = color.RGBA{
			R: uint8(raw[i*3+0]),
			G: uint8(raw[i*3+1]),
			B: uint8(raw[i*3+2]),
			A: 255,
		}
	}
	return p
}

func renderMap(s unsafe.Pointer, h hit, palette [256]color.RGBA, path string) error {
	// Allocate the grid in Go and hand C a pointer to it. Because the slice is
	// only borrowed for the duration of the call and C keeps no reference to
	// it, this is safe under cgo's pointer rules.
	grid := make([]C.int, mapSize*mapSize)
	C.scanner_biome_grid(s, C.uint64_t(h.seed), mapSize, mapStep, mapY,
		(*C.int)(unsafe.Pointer(&grid[0])))

	img := image.NewRGBA(image.Rect(0, 0, mapSize, mapSize))
	for j := 0; j < mapSize; j++ {
		for i := 0; i < mapSize; i++ {
			id := int(grid[j*mapSize+i])
			if id < 0 || id > 255 {
				id = 0
			}
			img.SetRGBA(i, j, palette[id])
		}
	}

	// Mark spawn with a small white cross.
	half := mapSize / 2
	sx := half + h.spawnX/mapStep
	sz := half + h.spawnZ/mapStep
	white := color.RGBA{255, 255, 255, 255}
	for d := -4; d <= 4; d++ {
		img.SetRGBA(sx+d, sz, white)
		img.SetRGBA(sx, sz+d, white)
	}

	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()
	return png.Encode(f, img)
}
