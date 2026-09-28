// ABOUTME: Maps tt7d touch events (logical 1280x800 x/y) onto one mouse: press, move, release.
// ABOUTME: The first finger down drives the mouse until it lifts; other fingers are ignored (no gestures in S1).
package screen

import "github.com/chromedp/cdproto/input"

const logicalW, logicalH = 1280, 800

// Touch is one tt7d touch event: action down/move/up, the pointer id, logical x/y.
type Touch struct {
	Action  string
	Pointer int
	X, Y    float64
}

// Mouse is one CDP Input.dispatchMouseEvent to send (left button, clickCount 1).
type Mouse struct {
	Type input.MouseType
	X, Y float64
}

// TouchMapper follows the finger that holds the mouse.
type TouchMapper struct {
	down    bool
	pointer int
	x, y    float64
}

// Map turns a touch into a mouse event; ok is false when the touch is ignored.
func (m *TouchMapper) Map(t Touch) (Mouse, bool) {
	x, y := clamp(t.X, logicalW-1), clamp(t.Y, logicalH-1)
	switch {
	case t.Action == "down" && !m.down:
		m.down, m.pointer = true, t.Pointer
		m.x, m.y = x, y
		return Mouse{input.MousePressed, x, y}, true
	case !m.down || t.Pointer != m.pointer:
		return Mouse{}, false
	case t.Action == "move":
		m.x, m.y = x, y
		return Mouse{input.MouseMoved, x, y}, true
	case t.Action == "up":
		m.down = false
		return Mouse{input.MouseReleased, x, y}, true
	}
	return Mouse{}, false
}

// Release lets go of a held mouse button, at the finger's last position, when
// the event stream drops mid-touch.
func (m *TouchMapper) Release() (Mouse, bool) {
	if !m.down {
		return Mouse{}, false
	}
	m.down = false
	return Mouse{input.MouseReleased, m.x, m.y}, true
}

func clamp(v, hi float64) float64 {
	return max(0, min(v, hi))
}
