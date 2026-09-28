// ABOUTME: The body of tt7d's PATCH /api/v1/frame (SPEC §10.1): a "TT7R" header, one 12-byte record per region,
// ABOUTME: then each region's PNG. Also the fast PNG settings for regions and the patched frame's SHA-256.
package regions

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"strings"
)

const (
	ContentType = "application/x-tt7-regions"
	Version     = 1
	MaxRegions  = 16 // tt7d refuses more in one batch
	headLen     = 8  // "TT7R", version, reserved 0, count (u16)
	recordLen   = 12 // x, y, w, h (u16 each), png_len (u32); big-endian
)

// Region is one rectangle of the logical screen and its pixels as a PNG of exactly that size.
type Region struct {
	Rect image.Rectangle
	PNG  []byte
}

// Encode builds a request body from regions, in order (tt7d applies them in order; later wins).
func Encode(rs []Region) ([]byte, error) {
	if len(rs) > MaxRegions {
		return nil, fmt.Errorf("%d regions: at most %d per batch", len(rs), MaxRegions)
	}
	var b bytes.Buffer
	b.WriteString("TT7R")
	b.WriteByte(Version)
	b.WriteByte(0)
	binary.Write(&b, binary.BigEndian, uint16(len(rs)))
	for _, r := range rs {
		if r.Rect.Empty() || r.Rect.Min.X < 0 || r.Rect.Min.Y < 0 || r.Rect.Max.X > 0xFFFF || r.Rect.Max.Y > 0xFFFF {
			return nil, fmt.Errorf("region %v cannot be encoded", r.Rect)
		}
		binary.Write(&b, binary.BigEndian, [4]uint16{uint16(r.Rect.Min.X), uint16(r.Rect.Min.Y), uint16(r.Rect.Dx()), uint16(r.Rect.Dy())})
		binary.Write(&b, binary.BigEndian, uint32(len(r.PNG)))
	}
	for _, r := range rs {
		b.Write(r.PNG)
	}
	return b.Bytes(), nil
}

// Parse splits a body back into regions. It checks the framing only (tt7d checks bounds and PNGs).
func Parse(body []byte) ([]Region, error) {
	if len(body) < headLen || string(body[:4]) != "TT7R" {
		return nil, errors.New("not a TT7R container")
	}
	if body[4] != Version || body[5] != 0 {
		return nil, fmt.Errorf("version %d, reserved %d", body[4], body[5])
	}
	n := int(binary.BigEndian.Uint16(body[6:]))
	off := headLen + n*recordLen
	if n > MaxRegions || len(body) < off {
		return nil, fmt.Errorf("%d regions in %d bytes", n, len(body))
	}
	rs := make([]Region, n)
	for i := range rs {
		rec := body[headLen+i*recordLen:]
		x, y := int(binary.BigEndian.Uint16(rec)), int(binary.BigEndian.Uint16(rec[2:]))
		w, h := int(binary.BigEndian.Uint16(rec[4:])), int(binary.BigEndian.Uint16(rec[6:]))
		size := int(binary.BigEndian.Uint32(rec[8:]))
		if size > len(body)-off {
			return nil, fmt.Errorf("region %d's PNG runs past the body", i)
		}
		rs[i] = Region{Rect: image.Rect(x, y, x+w, y+h), PNG: body[off : off+size]}
		off += size
	}
	if off != len(body) {
		return nil, fmt.Errorf("%d bytes after the last PNG", len(body)-off)
	}
	return rs, nil
}

// FrameSHA is the SHA-256 tt7d reports for the frame a PATCH produces: sha256 of the base frame's
// SHA-256 as 64 lowercase hex digits, followed by the whole request body. For a full PUT frame
// tt7d reports sha256 of the PNG itself. So both sides know it without hashing 4 MB of pixels.
func FrameSHA(baseSHA string, body []byte) string {
	h := sha256.New()
	h.Write([]byte(strings.ToLower(baseSHA)))
	h.Write(body)
	return hex.EncodeToString(h.Sum(nil))
}

// pngEncoder trades size for speed: region PNGs are small, and they are encoded on the touch path
// (BenchmarkEncodePNG compares the levels).
var pngEncoder = png.Encoder{CompressionLevel: png.BestSpeed}

// EncodePNG encodes rect r of img as a PNG of r's size. Opaque images come out as 8-bit RGB.
func EncodePNG(img *image.NRGBA, r image.Rectangle) ([]byte, error) {
	var b bytes.Buffer
	if err := pngEncoder.Encode(&b, img.SubImage(r)); err != nil {
		return nil, err
	}
	return b.Bytes(), nil
}

// ToNRGBA returns img as non-premultiplied 8-bit RGBA with its origin at (0, 0): the bytes lodepng's
// decode32 gives tt7d for the same PNG. Chrome's opaque captures copy straight across.
func ToNRGBA(img image.Image) *image.NRGBA {
	b := img.Bounds()
	if n, ok := img.(*image.NRGBA); ok && b.Min == (image.Point{}) {
		return n
	}
	out := image.NewNRGBA(image.Rect(0, 0, b.Dx(), b.Dy()))
	if rgba, ok := img.(*image.RGBA); ok && opaque(rgba) { // premultiplied equals straight at alpha 255
		for y := 0; y < b.Dy(); y++ {
			copy(out.Pix[y*out.Stride:y*out.Stride+b.Dx()*4], rgba.Pix[(y)*rgba.Stride:])
		}
		return out
	}
	for y := b.Min.Y; y < b.Max.Y; y++ {
		for x := b.Min.X; x < b.Max.X; x++ {
			out.SetNRGBA(x-b.Min.X, y-b.Min.Y, color.NRGBAModel.Convert(img.At(x, y)).(color.NRGBA))
		}
	}
	return out
}

func opaque(img *image.RGBA) bool {
	for i := 3; i < len(img.Pix); i += 4 {
		if img.Pix[i] != 255 {
			return false
		}
	}
	return true
}
