// ABOUTME: Tests for touch -> mouse mapping: the first finger drives the mouse, others are ignored,
// ABOUTME: coordinates are clamped to 1280x800, and a dropped stream releases a held button.
package screen

import (
	"testing"

	"github.com/chromedp/cdproto/input"
)

func TestTouchMapping(t *testing.T) {
	var m TouchMapper
	steps := []struct {
		in   Touch
		ok   bool
		want Mouse
	}{
		{Touch{"down", 0, 100, 200}, true, Mouse{input.MousePressed, 100, 200}},
		{Touch{"down", 3, 900, 700}, false, Mouse{}}, // second finger: ignored
		{Touch{"move", 3, 910, 700}, false, Mouse{}},
		{Touch{"move", 0, 110, 210}, true, Mouse{input.MouseMoved, 110, 210}},
		{Touch{"up", 3, 910, 700}, false, Mouse{}},
		{Touch{"up", 0, 120, 220}, true, Mouse{input.MouseReleased, 120, 220}},
		{Touch{"move", 0, 1, 1}, false, Mouse{}},                               // no finger down any more
		{Touch{"down", 5, 2000, -4}, true, Mouse{input.MousePressed, 1279, 0}}, // clamped
		{Touch{"up", 5, 2000, -4}, true, Mouse{input.MouseReleased, 1279, 0}},
		{Touch{"wiggle", 0, 1, 1}, false, Mouse{}},
	}
	for i, s := range steps {
		got, ok := m.Map(s.in)
		if ok != s.ok || got != s.want {
			t.Errorf("step %d %+v: got %+v %v, want %+v %v", i, s.in, got, ok, s.want, s.ok)
		}
	}
}

func TestReleaseOnStreamLoss(t *testing.T) {
	var m TouchMapper
	if _, ok := m.Release(); ok {
		t.Error("nothing held: Release should do nothing")
	}
	m.Map(Touch{"down", 2, 50, 60})
	m.Map(Touch{"move", 2, 55, 65})
	got, ok := m.Release()
	if !ok || got != (Mouse{input.MouseReleased, 55, 65}) {
		t.Errorf("Release: %+v %v", got, ok)
	}
	if _, ok := m.Map(Touch{"up", 2, 55, 65}); ok {
		t.Error("an up after Release should be ignored")
	}
}
