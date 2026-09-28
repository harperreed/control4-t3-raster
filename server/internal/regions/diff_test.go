// ABOUTME: Tests for the tile diff: known image pairs give known rectangles, every changed pixel is covered,
// ABOUTME: at most maxRects come back, and a large change or an unknown base asks for a full frame.
package regions

import (
	"image"
	"image/color"
	"math/rand"
	"testing"
)

const w, h = 1280, 800

// base is a busy image: every pixel differs from its neighbours.
func base() *image.NRGBA {
	img := image.NewNRGBA(image.Rect(0, 0, w, h))
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			img.SetNRGBA(x, y, color.NRGBA{uint8(x), uint8(y), uint8(x + y), 255})
		}
	}
	return img
}

func clone(img *image.NRGBA) *image.NRGBA {
	c := image.NewNRGBA(img.Rect)
	copy(c.Pix, img.Pix)
	return c
}

func fill(img *image.NRGBA, r image.Rectangle, c color.NRGBA) {
	for y := r.Min.Y; y < r.Max.Y; y++ {
		for x := r.Min.X; x < r.Max.X; x++ {
			img.SetNRGBA(x, y, c)
		}
	}
}

// covered fails the test for any pixel that differs but lies in no rectangle.
func covered(t *testing.T, old, cur *image.NRGBA, rects []image.Rectangle) {
	t.Helper()
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			if old.NRGBAAt(x, y) == cur.NRGBAAt(x, y) {
				continue
			}
			in := false
			for _, r := range rects {
				in = in || image.Pt(x, y).In(r)
			}
			if !in {
				t.Fatalf("changed pixel (%d,%d) is in no rectangle of %v", x, y, rects)
			}
		}
	}
}

func TestIdenticalGivesNothing(t *testing.T) {
	b := base()
	if rects := DirtyRects(b, clone(b), 32, 4); len(rects) != 0 {
		t.Errorf("identical images: %v", rects)
	}
}

func TestOneSmallChangeIsOneTile(t *testing.T) {
	old := base()
	cur := clone(old)
	cur.SetNRGBA(100, 100, color.NRGBA{1, 2, 3, 255})
	rects := DirtyRects(old, cur, 32, 4)
	if want := image.Rect(96, 96, 128, 128); len(rects) != 1 || rects[0] != want {
		t.Errorf("one pixel: %v, want [%v]", rects, want)
	}
}

func TestButtonPressIsOneRect(t *testing.T) {
	old := base()
	cur := clone(old)
	fill(cur, image.Rect(1000, 650, 1200, 750), color.NRGBA{0, 255, 0, 255}) // a 200x100 button
	rects := DirtyRects(old, cur, 32, 4)
	if want := image.Rect(992, 640, 1216, 768); len(rects) != 1 || rects[0] != want {
		t.Errorf("button: %v, want [%v]", rects, want)
	}
	covered(t, old, cur, rects)
}

func TestTwoFarChangesAreTwoRects(t *testing.T) {
	old := base()
	cur := clone(old)
	fill(cur, image.Rect(10, 10, 50, 30), color.NRGBA{255, 0, 0, 255})
	fill(cur, image.Rect(1200, 700, 1270, 790), color.NRGBA{0, 0, 255, 255})
	rects := DirtyRects(old, cur, 32, 4)
	if len(rects) != 2 {
		t.Fatalf("two far changes: %v", rects)
	}
	covered(t, old, cur, rects)
	area := 0
	for _, r := range rects {
		area += r.Dx() * r.Dy()
	}
	if area != (2+3*4)*32*32 { // their own tiles (2 + 12), far less than their joint bounding box
		t.Errorf("two small changes cost %d px: %v", area, rects)
	}
}

func TestAtMostMaxRectsAndClosestMerged(t *testing.T) {
	old := base()
	cur := clone(old)
	// Six separate changes; the two at the top left are closest and must share a rectangle.
	for _, p := range []image.Point{{0, 0}, {70, 0}, {600, 400}, {1270, 0}, {0, 790}, {1270, 790}} {
		cur.SetNRGBA(p.X, p.Y, color.NRGBA{9, 9, 9, 255})
	}
	rects := DirtyRects(old, cur, 32, 4)
	if len(rects) > 4 {
		t.Fatalf("%d rects: %v", len(rects), rects)
	}
	covered(t, old, cur, rects)
	together := false
	for _, r := range rects {
		together = together || (image.Pt(0, 0).In(r) && image.Pt(70, 0).In(r))
	}
	if !together {
		t.Errorf("the two closest changes were not merged: %v", rects)
	}
}

func TestFaintEdgeChangesAreCovered(t *testing.T) {
	// Antialiasing: a text edge that moves changes pixels by a level or two, right on a tile border.
	// The diff is exact, so any changed pixel counts, however faint, and its tile covers it.
	old := base()
	cur := clone(old)
	for y := 200; y < 240; y++ {
		c := old.NRGBAAt(31, y)
		c.R++
		cur.SetNRGBA(31, y, c) // last column of a tile
		d := old.NRGBAAt(32, y)
		d.B--
		cur.SetNRGBA(32, y, d) // first column of the next
	}
	rects := DirtyRects(old, cur, 32, 4)
	covered(t, old, cur, rects)
	if len(rects) != 1 || rects[0] != image.Rect(0, 192, 64, 256) {
		t.Errorf("faint edge: %v", rects)
	}
}

func TestEdgesClipToTheImage(t *testing.T) {
	old := image.NewNRGBA(image.Rect(0, 0, 100, 50)) // not a multiple of the tile size
	cur := clone(old)
	cur.SetNRGBA(99, 49, color.NRGBA{1, 1, 1, 1})
	rects := DirtyRects(old, cur, 32, 4)
	if want := image.Rect(96, 32, 100, 50); len(rects) != 1 || rects[0] != want {
		t.Errorf("corner: %v, want [%v]", rects, want)
	}
}

func TestRandomChangesAlwaysCovered(t *testing.T) {
	rng := rand.New(rand.NewSource(7))
	old := base()
	for round := 0; round < 20; round++ {
		cur := clone(old)
		for i := 0; i < 1+rng.Intn(12); i++ {
			x, y := rng.Intn(w), rng.Intn(h)
			fill(cur, image.Rect(x, y, min(w, x+1+rng.Intn(200)), min(h, y+1+rng.Intn(150))), color.NRGBA{uint8(i), 7, 7, 255})
		}
		rects := DirtyRects(old, cur, 32, 4)
		if len(rects) > 4 {
			t.Fatalf("round %d: %d rects", round, len(rects))
		}
		for i := range rects {
			for j := i + 1; j < len(rects); j++ {
				if rects[i].Overlaps(rects[j]) {
					t.Fatalf("round %d: %v and %v overlap", round, rects[i], rects[j])
				}
			}
		}
		covered(t, old, cur, rects)
	}
}

func TestPlanChoosesFullOrRegions(t *testing.T) {
	old := base()
	small := clone(old)
	fill(small, image.Rect(1000, 650, 1200, 750), color.NRGBA{0, 255, 0, 255})
	big := clone(old)
	fill(big, image.Rect(0, 0, 1280, 450), color.NRGBA{0, 0, 0, 255}) // 56% of the screen

	if rects, full := Plan(old, small, 0.5); full || len(rects) != 1 {
		t.Errorf("button press: full %v rects %v", full, rects)
	}
	if _, full := Plan(old, big, 0.5); !full {
		t.Errorf("56%% changed should be a full frame")
	}
	if _, full := Plan(nil, small, 0.5); !full {
		t.Errorf("unknown base should be a full frame")
	}
	if _, full := Plan(old, small, 0); !full {
		t.Errorf("max fraction 0 turns regions off")
	}
	if rects, full := Plan(old, clone(old), 0.5); full || len(rects) != 0 {
		t.Errorf("no change: full %v rects %v", full, rects)
	}
}
