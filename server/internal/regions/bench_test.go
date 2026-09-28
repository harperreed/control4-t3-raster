// ABOUTME: Benchmarks for the region path: the tile diff over a full 1280x800 capture, and region PNG
// ABOUTME: encoding at each zlib level (why EncodePNG uses BestSpeed). Run: go test -bench . ./internal/regions
package regions

import (
	"bytes"
	"image"
	"image/color"
	"image/png"
	"testing"
)

// buttonRegion is a 256x128 region like a pressed button: flat colour with a few text-like stripes.
func buttonRegion() *image.NRGBA {
	img := solid(image.Rect(0, 0, 256, 128), color.NRGBA{30, 90, 200, 255})
	for y := 40; y < 90; y += 6 {
		fill(img, image.Rect(40, y, 216, y+3), color.NRGBA{255, 255, 255, 255})
	}
	return img
}

func BenchmarkEncodePNG(b *testing.B) {
	img := buttonRegion()
	for _, lvl := range []struct {
		name string
		l    png.CompressionLevel
	}{{"BestSpeed", png.BestSpeed}, {"Default", png.DefaultCompression}, {"BestCompression", png.BestCompression}} {
		b.Run(lvl.name, func(b *testing.B) {
			enc := png.Encoder{CompressionLevel: lvl.l}
			var n int
			for b.Loop() {
				var buf bytes.Buffer
				enc.Encode(&buf, img)
				n = buf.Len()
			}
			b.ReportMetric(float64(n), "bytes")
		})
	}
}

func BenchmarkDirtyRects(b *testing.B) {
	old := base()
	cur := clone(old)
	fill(cur, image.Rect(1000, 650, 1200, 750), color.NRGBA{0, 255, 0, 255})
	for b.Loop() {
		DirtyRects(old, cur, TileSize, MaxRects)
	}
}
