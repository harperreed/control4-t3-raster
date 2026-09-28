// ABOUTME: Which parts of the screen changed between two captures: exact per-pixel compare in square tiles,
// ABOUTME: changed tiles grouped and merged into at most a few rectangles, or "send a full frame" when that is cheaper.
package regions

import (
	"bytes"
	"image"
)

const (
	TileSize = 32 // pixels per side of a diff tile
	MaxRects = 4  // rectangles per update; more changes are merged into these
)

// Plan decides how to send cur when the panel shows old (nil: unknown). full is true when a full frame
// should go: the base is unknown, maxFraction is 0 (regions off), or the rectangles cover more than
// maxFraction of the screen. Otherwise rects (possibly none: nothing changed) are the regions to send.
func Plan(old, cur *image.NRGBA, maxFraction float64) (rects []image.Rectangle, full bool) {
	if old == nil || maxFraction <= 0 || old.Rect != cur.Rect {
		return nil, true
	}
	rects = DirtyRects(old, cur, TileSize, MaxRects)
	area := 0
	for _, r := range rects {
		area += r.Dx() * r.Dy()
	}
	if float64(area) > maxFraction*float64(cur.Rect.Dx()*cur.Rect.Dy()) {
		return nil, true
	}
	return rects, false
}

// DirtyRects returns at most maxRects non-overlapping, tile-aligned rectangles (clipped to the image)
// that cover every pixel where old and cur differ. Both images must have the same bounds, origin (0, 0).
//
// The compare is exact, so an antialiased edge that moved by one level still counts; no padding is
// needed. Changed tiles that touch (8-neighbours) form one group; each group becomes its bounding box.
// Then, while boxes overlap or there are more than maxRects, the pair whose union adds the least
// unchanged area is merged.
func DirtyRects(old, cur *image.NRGBA, tile, maxRects int) []image.Rectangle {
	W, H := cur.Rect.Dx(), cur.Rect.Dy()
	tw, th := (W+tile-1)/tile, (H+tile-1)/tile
	dirty := make([]bool, tw*th)
	for y := 0; y < H; y++ {
		a := old.Pix[y*old.Stride : y*old.Stride+W*4]
		b := cur.Pix[y*cur.Stride : y*cur.Stride+W*4]
		if bytes.Equal(a, b) {
			continue
		}
		row := dirty[(y/tile)*tw:]
		for tx := 0; tx < tw; tx++ {
			x0, x1 := tx*tile*4, min((tx+1)*tile, W)*4
			if !row[tx] && !bytes.Equal(a[x0:x1], b[x0:x1]) {
				row[tx] = true
			}
		}
	}

	// Group touching tiles (flood fill), in tile units.
	var boxes []image.Rectangle
	seen := make([]bool, len(dirty))
	for i, d := range dirty {
		if !d || seen[i] {
			continue
		}
		box := image.Rect(i%tw, i/tw, i%tw+1, i/tw+1)
		stack := []int{i}
		seen[i] = true
		for len(stack) > 0 {
			j := stack[len(stack)-1]
			stack = stack[:len(stack)-1]
			x, y := j%tw, j/tw
			box = box.Union(image.Rect(x, y, x+1, y+1))
			for dy := -1; dy <= 1; dy++ {
				for dx := -1; dx <= 1; dx++ {
					nx, ny := x+dx, y+dy
					if nx < 0 || ny < 0 || nx >= tw || ny >= th {
						continue
					}
					k := ny*tw + nx
					if dirty[k] && !seen[k] {
						seen[k] = true
						stack = append(stack, k)
					}
				}
			}
		}
		boxes = append(boxes, box)
	}

	for {
		bi, bj, best := -1, -1, 0
		for i := range boxes {
			for j := i + 1; j < len(boxes); j++ {
				overlap := boxes[i].Overlaps(boxes[j])
				if !overlap && len(boxes) <= maxRects {
					continue
				}
				u := boxes[i].Union(boxes[j])
				cost := area(u) - area(boxes[i]) - area(boxes[j])
				if overlap {
					cost = -1 << 30 // always merge overlapping boxes first
				}
				if bi < 0 || cost < best {
					bi, bj, best = i, j, cost
				}
			}
		}
		if bi < 0 {
			break
		}
		boxes[bi] = boxes[bi].Union(boxes[bj])
		boxes = append(boxes[:bj], boxes[bj+1:]...)
	}

	bounds := image.Rect(0, 0, W, H)
	out := make([]image.Rectangle, len(boxes))
	for i, b := range boxes {
		out[i] = image.Rect(b.Min.X*tile, b.Min.Y*tile, b.Max.X*tile, b.Max.Y*tile).Intersect(bounds)
	}
	return out
}

func area(r image.Rectangle) int { return r.Dx() * r.Dy() }
