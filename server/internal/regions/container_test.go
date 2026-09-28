// ABOUTME: Tests for the PATCH /frame container: encode/parse round trip, refusals, the frame SHA-256, and the
// ABOUTME: golden vector in tt7d/test/fixtures that tt7d's C test (test_regions.c) checks too.
package regions

import (
	"bufio"
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"flag"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

var update = flag.Bool("update", false, "rewrite the golden vector in tt7d/test/fixtures")

var goldenDir = filepath.Join("..", "..", "..", "tt7d", "test", "fixtures")

func solid(r image.Rectangle, c color.NRGBA) *image.NRGBA {
	img := image.NewNRGBA(r)
	fill(img, r, c)
	return img
}

func TestEncodeParseRoundTrip(t *testing.T) {
	img := solid(image.Rect(0, 0, w, h), color.NRGBA{1, 2, 3, 255})
	var rs []Region
	for _, r := range []image.Rectangle{image.Rect(10, 20, 40, 60), image.Rect(1270, 790, 1280, 800)} {
		p, err := EncodePNG(img, r)
		if err != nil {
			t.Fatal(err)
		}
		rs = append(rs, Region{Rect: r, PNG: p})
	}
	body, err := Encode(rs)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(body[:8], []byte{'T', 'T', '7', 'R', 1, 0, 0, 2}) {
		t.Errorf("header % x", body[:8])
	}
	// Record 0 by hand: x=10, y=20, w=30, h=40, then the PNG's length, all big-endian.
	rec := body[8:20]
	if want := []byte{0, 10, 0, 20, 0, 30, 0, 40}; !bytes.Equal(rec[:8], want) {
		t.Errorf("record 0 % x, want % x", rec[:8], want)
	}
	got, err := Parse(body)
	if err != nil {
		t.Fatal(err)
	}
	if len(got) != 2 || got[0].Rect != rs[0].Rect || got[1].Rect != rs[1].Rect ||
		!bytes.Equal(got[0].PNG, rs[0].PNG) || !bytes.Equal(got[1].PNG, rs[1].PNG) {
		t.Errorf("round trip changed the regions")
	}
	empty, _ := Encode(nil)
	if !bytes.Equal(empty, []byte{'T', 'T', '7', 'R', 1, 0, 0, 0}) {
		t.Errorf("empty batch % x", empty)
	}
}

func TestEncodeRefuses(t *testing.T) {
	many := make([]Region, MaxRegions+1)
	for i := range many {
		many[i] = Region{Rect: image.Rect(0, 0, 1, 1), PNG: []byte("x")}
	}
	if _, err := Encode(many); err == nil {
		t.Error("17 regions encoded")
	}
	for _, r := range []image.Rectangle{image.Rect(0, 0, 0, 5), image.Rect(-1, 0, 5, 5), image.Rect(0, 0, 70000, 5)} {
		if _, err := Encode([]Region{{Rect: r, PNG: []byte("x")}}); err == nil {
			t.Errorf("rect %v encoded", r)
		}
	}
}

func TestParseRefuses(t *testing.T) {
	good, _ := Encode([]Region{{Rect: image.Rect(0, 0, 2, 2), PNG: []byte("abcd")}})
	for name, body := range map[string][]byte{
		"short":    good[:7],
		"magic":    append([]byte("XX7R"), good[4:]...),
		"version":  append(append([]byte{}, good[:4]...), append([]byte{2}, good[5:]...)...),
		"truncate": good[:len(good)-1],
		"trailing": append(append([]byte{}, good...), 0),
	} {
		if _, err := Parse(body); err == nil {
			t.Errorf("%s: parsed", name)
		}
	}
}

func TestFrameSHA(t *testing.T) {
	// The same known answer as tt7d/test_regions.c (sha256sum over the lowercase hex, then the body).
	got := FrameSHA("00112233445566778899aabbccddeeff00112233445566778899AABBCCDDEEFF", []byte{'T', 'T', '7', 'R', 1, 0, 0, 0})
	if want := "017c22fc77a67a73a7f1eecbd07b60c0e76e9cf8d951bd7ef0d92cba90c6a0c5"; got != want {
		t.Errorf("FrameSHA %s, want %s", got, want)
	}
}

// ---- the golden vector -------------------------------------------------------

// goldenBase is pixel (x, y) = (x mod 256, y mod 256, (x+y) mod 256, 255), as in test_regions.c.
func goldenBase() *image.NRGBA { return base() }

// goldenRegions: two inverted corners, then a solid block over part of the first (later wins).
func goldenRegions() []struct {
	r      image.Rectangle
	invert bool
	c      color.NRGBA
} {
	return []struct {
		r      image.Rectangle
		invert bool
		c      color.NRGBA
	}{
		{image.Rect(0, 0, 64, 32), true, color.NRGBA{}},
		{image.Rect(1216, 768, 1280, 800), true, color.NRGBA{}},
		{image.Rect(32, 16, 80, 48), false, color.NRGBA{10, 20, 30, 255}},
	}
}

const goldenBaseSHA = "5a1f0c2e9d8b7a6f5e4d3c2b1a09f8e7d6c5b4a39281706f5e4d3c2b1a090807"

// goldenContent is region i's pixels, in image coordinates.
func goldenContent(i int) *image.NRGBA {
	g := goldenRegions()[i]
	img := image.NewNRGBA(g.r)
	b := goldenBase()
	for y := g.r.Min.Y; y < g.r.Max.Y; y++ {
		for x := g.r.Min.X; x < g.r.Max.X; x++ {
			c := g.c
			if g.invert {
				o := b.NRGBAAt(x, y)
				c = color.NRGBA{255 - o.R, 255 - o.G, 255 - o.B, 255}
			}
			img.SetNRGBA(x, y, c)
		}
	}
	return img
}

// goldenResult applies the regions to the base in order, independently of any decoder.
func goldenResult() *image.NRGBA {
	img := goldenBase()
	for i, g := range goldenRegions() {
		c := goldenContent(i)
		for y := g.r.Min.Y; y < g.r.Max.Y; y++ {
			for x := g.r.Min.X; x < g.r.Max.X; x++ {
				img.SetNRGBA(x, y, c.NRGBAAt(x, y))
			}
		}
	}
	return img
}

func readGoldenText(t *testing.T) map[string]string {
	t.Helper()
	f, err := os.Open(filepath.Join(goldenDir, "regions-v1.txt"))
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	out := map[string]string{}
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		if k, v, ok := strings.Cut(sc.Text(), "="); ok && !strings.HasPrefix(k, "#") {
			out[k] = v
		}
	}
	return out
}

func TestGoldenVector(t *testing.T) {
	binPath := filepath.Join(goldenDir, "regions-v1.bin")
	resultSum := sha256.Sum256(goldenResult().Pix)
	if *update {
		var rs []Region
		for i, g := range goldenRegions() {
			p, err := EncodePNG(goldenContent(i), g.r)
			if err != nil {
				t.Fatal(err)
			}
			rs = append(rs, Region{Rect: g.r, PNG: p})
		}
		body, err := Encode(rs)
		if err != nil {
			t.Fatal(err)
		}
		text := fmt.Sprintf(`# Golden vector for PATCH /api/v1/frame (SPEC §10.1): regions-v1.bin is one request body.
# Written by: cd server && go test ./internal/regions -run TestGoldenVector -update
# Checked by: server/internal/regions/container_test.go and tt7d/test_regions.c.
# The base frame is 1280x800 with pixel (x, y) = RGBA(x mod 256, y mod 256, (x + y) mod 256, 255).
# Regions, in order: (0,0) 64x32 and (1216,768) 64x32 hold the base's colours inverted (255 - c, alpha 255);
# (32,16) 48x32 is solid RGBA(10, 20, 30, 255) and covers part of the first (later wins).
# result_rgba_sha256: sha256 of the 1280x800 RGBA bytes (row-major) after applying the regions to the base.
# frame_sha256: the patched frame's sha256 when the base frame's sha256 is base_frame_sha256.
regions=%d
base_frame_sha256=%s
result_rgba_sha256=%s
frame_sha256=%s
`, len(rs), goldenBaseSHA, hex.EncodeToString(resultSum[:]), FrameSHA(goldenBaseSHA, body))
		if err := os.WriteFile(binPath, body, 0o644); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(goldenDir, "regions-v1.txt"), []byte(text), 0o644); err != nil {
			t.Fatal(err)
		}
	}

	body, err := os.ReadFile(binPath)
	if err != nil {
		t.Fatal(err)
	}
	text := readGoldenText(t)
	rs, err := Parse(body)
	if err != nil {
		t.Fatal(err)
	}
	if fmt.Sprint(len(rs)) != text["regions"] || len(rs) != len(goldenRegions()) {
		t.Fatalf("%d regions, text says %s", len(rs), text["regions"])
	}
	// Framing: the encoder rebuilds the file byte for byte from its own PNGs.
	again, err := Encode(rs)
	if err != nil || !bytes.Equal(again, body) {
		t.Fatalf("Encode(Parse(golden)) differs from the golden file (err %v)", err)
	}
	// Content: each PNG decodes to exactly its region of the expected image.
	for i, r := range rs {
		if r.Rect != goldenRegions()[i].r {
			t.Errorf("region %d rect %v", i, r.Rect)
		}
		img, err := png.Decode(bytes.NewReader(r.PNG))
		if err != nil {
			t.Fatalf("region %d: %v", i, err)
		}
		got, want := ToNRGBA(img), goldenContent(i)
		if got.Rect.Dx() != want.Rect.Dx() || got.Rect.Dy() != want.Rect.Dy() {
			t.Fatalf("region %d is %v", i, got.Rect)
		}
		for y := 0; y < got.Rect.Dy(); y++ {
			for x := 0; x < got.Rect.Dx(); x++ {
				if got.NRGBAAt(x, y) != want.NRGBAAt(want.Rect.Min.X+x, want.Rect.Min.Y+y) {
					t.Fatalf("region %d pixel (%d,%d) differs", i, x, y)
				}
			}
		}
	}
	if text["base_frame_sha256"] != goldenBaseSHA {
		t.Errorf("base sha in the text: %s", text["base_frame_sha256"])
	}
	if got := hex.EncodeToString(resultSum[:]); got != text["result_rgba_sha256"] {
		t.Errorf("result pixels hash %s, text says %s", got, text["result_rgba_sha256"])
	}
	if got := FrameSHA(goldenBaseSHA, body); got != text["frame_sha256"] {
		t.Errorf("frame sha %s, text says %s", got, text["frame_sha256"])
	}
}
