// ABOUTME: Tests for the frame pacer: identical frames are skipped, pushes are spaced by 1/max_fps,
// ABOUTME: and Forget makes the next frame go even when it is identical (panel restarted).
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

	if send, wait := p.Check(t0, a); !send || wait != 0 {
		t.Fatalf("first frame: send %v wait %v", send, wait)
	}
	p.Sent(t0, a)
	if send, wait := p.Check(t0.Add(time.Second), a); send || wait != 0 {
		t.Errorf("identical frame: send %v wait %v, want skipped", send, wait)
	}
	if send, wait := p.Check(t0.Add(50*time.Millisecond), b); send || wait != 150*time.Millisecond {
		t.Errorf("changed frame 50 ms later: send %v wait %v, want wait 150ms", send, wait)
	}
	if send, _ := p.Check(t0.Add(200*time.Millisecond), b); !send {
		t.Errorf("changed frame 200 ms later should go")
	}
	p.Sent(t0.Add(200*time.Millisecond), b)
	p.Forget()
	if send, wait := p.Check(t0.Add(250*time.Millisecond), b); send || wait != 150*time.Millisecond {
		t.Errorf("after Forget, identical frame inside the interval: send %v wait %v, want wait", send, wait)
	}
	if send, _ := p.Check(t0.Add(400*time.Millisecond), b); !send {
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
		if send, _ := p.Check(now, h); send {
			p.Sent(now, h)
			sent++
		}
	}
	if sent != 5 {
		t.Errorf("%d frames in one second at max_fps 5", sent)
	}
}
