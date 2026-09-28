// ABOUTME: Tests for the frame pacer: identical frames are skipped, pushes are spaced by 1/max_fps,
// ABOUTME: Forget makes the next frame go even when identical, and the captures right after a touch skip the wait.
package screen

import (
	"crypto/sha256"
	"testing"
	"time"
)

func TestPacer(t *testing.T) {
	p := NewPacer(5) // 200 ms apart
	t0 := time.Date(2026, 9, 28, 12, 0, 0, 0, time.UTC)
	a, b := sha256.Sum256([]byte("a")), sha256.Sum256([]byte("b"))

	if send, wait := p.Check(t0, a, false); !send || wait != 0 {
		t.Fatalf("first frame: send %v wait %v", send, wait)
	}
	p.Sent(t0, a)
	if send, wait := p.Check(t0.Add(time.Second), a, false); send || wait != 0 {
		t.Errorf("identical frame: send %v wait %v, want skipped", send, wait)
	}
	if send, wait := p.Check(t0.Add(50*time.Millisecond), b, false); send || wait != 150*time.Millisecond {
		t.Errorf("changed frame 50 ms later: send %v wait %v, want wait 150ms", send, wait)
	}
	if send, _ := p.Check(t0.Add(200*time.Millisecond), b, false); !send {
		t.Errorf("changed frame 200 ms later should go")
	}
	p.Sent(t0.Add(200*time.Millisecond), b)
	p.Forget()
	if send, wait := p.Check(t0.Add(250*time.Millisecond), b, false); send || wait != 150*time.Millisecond {
		t.Errorf("after Forget, identical frame inside the interval: send %v wait %v, want wait", send, wait)
	}
	if send, _ := p.Check(t0.Add(400*time.Millisecond), b, false); !send {
		t.Errorf("after Forget, the identical frame should go once the interval passed")
	}
}

func TestPacerRateOverASecond(t *testing.T) {
	p := NewPacer(5)
	t0 := time.Date(2026, 9, 28, 12, 0, 0, 0, time.UTC)
	sent := 0
	for ms := 0; ms < 1000; ms += 10 { // a new frame every 10 ms, like a CSS animation
		now := t0.Add(time.Duration(ms) * time.Millisecond)
		h := sha256.Sum256([]byte{byte(ms), byte(ms >> 8)})
		if send, _ := p.Check(now, h, false); send {
			p.Sent(now, h)
			sent++
		}
	}
	if sent != 5 {
		t.Errorf("%d frames in one second at max_fps 5", sent)
	}
}

func TestUrgentSkipsTheWaitButNotDedup(t *testing.T) {
	p := NewPacer(5)
	t0 := time.Date(2026, 9, 28, 12, 0, 0, 0, time.UTC)
	a, b := sha256.Sum256([]byte("a")), sha256.Sum256([]byte("b"))
	p.Sent(t0, a)
	if send, wait := p.Check(t0.Add(20*time.Millisecond), b, true); !send || wait != 0 {
		t.Errorf("urgent changed frame 20 ms after a push: send %v wait %v, want now", send, wait)
	}
	if send, wait := p.Check(t0.Add(20*time.Millisecond), a, true); send || wait != 0 {
		t.Errorf("urgent identical frame: send %v wait %v, want skipped", send, wait)
	}
}

func TestBypassAfterTouch(t *testing.T) {
	var b Bypass
	t0 := time.Date(2026, 9, 28, 12, 0, 0, 0, time.UTC)
	if b.Capture(t0) {
		t.Error("a capture with no touch was urgent")
	}
	b.Touch(t0) // down
	if !b.Capture(t0.Add(10 * time.Millisecond)) {
		t.Error("the first capture after a touch should be urgent")
	}
	if b.Capture(t0.Add(20 * time.Millisecond)) {
		t.Error("one touch event makes one capture urgent")
	}
	b.Touch(t0.Add(100 * time.Millisecond)) // up, then a quick second tap: only 2 credits are kept
	b.Touch(t0.Add(110 * time.Millisecond))
	b.Touch(t0.Add(120 * time.Millisecond))
	n := 0
	for i := 0; i < 5; i++ {
		if b.Capture(t0.Add(130 * time.Millisecond)) {
			n++
		}
	}
	if n != 2 {
		t.Errorf("%d urgent captures after 3 touch events, want 2", n)
	}
	b.Touch(t0.Add(time.Second))
	if b.Capture(t0.Add(time.Second + BypassWindow)) {
		t.Error("a capture a full window after the touch is not its result")
	}
}
