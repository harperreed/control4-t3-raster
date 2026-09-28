// ABOUTME: Frame pacing for one screen: never push the same pixels twice in a row, never faster than max_fps,
// ABOUTME: except the captures right after a touch. Pure logic over a caller-supplied clock, tested without sleeping.
package screen

import "time"

// Pacer decides whether a captured frame goes to the panel now, later, or not at all.
type Pacer struct {
	interval time.Duration
	lastAt   time.Time
	lastHash [32]byte
	sent     bool // lastHash is what the panel shows
	pushed   bool // lastAt is set
}

func NewPacer(maxFPS int) *Pacer {
	return &Pacer{interval: time.Second / time.Duration(maxFPS)}
}

// Check says whether a frame with this hash may be pushed at now. When send is
// false and wait is 0 the frame matches what the panel shows: skip it. When wait
// is positive, check again after it. An urgent frame (the result of a touch)
// skips the wait, but a duplicate is still skipped.
func (p *Pacer) Check(now time.Time, hash [32]byte, urgent bool) (send bool, wait time.Duration) {
	if p.sent && hash == p.lastHash {
		return false, 0
	}
	if p.pushed && !urgent {
		if next := p.lastAt.Add(p.interval); now.Before(next) {
			return false, next.Sub(now)
		}
	}
	return true, 0
}

// Sent records a push the panel accepted.
func (p *Pacer) Sent(now time.Time, hash [32]byte) {
	p.lastAt, p.lastHash, p.sent, p.pushed = now, hash, true, true
}

// Forget drops what the panel is known to show (it restarted, or fell back to its
// clock), so the next frame goes even if identical. The rate limit still holds.
func (p *Pacer) Forget() {
	p.sent = false
}

// BypassWindow is how long after a touch event a capture still counts as its result.
const BypassWindow = time.Second

// Bypass marks the first capture after each touch down and each touch up as urgent, so a tap's
// feedback (the pressed look, then the result) reaches the panel without waiting out 1/max_fps.
// At most two are owed at once (one tap), and only within BypassWindow of the last touch event.
type Bypass struct {
	owed    int
	touchAt time.Time
}

// Touch records a touch down or up.
func (b *Bypass) Touch(now time.Time) {
	b.owed = min(b.owed+1, 2)
	b.touchAt = now
}

// Capture reports whether a capture arriving at now is urgent, and uses up one owed bypass if so.
func (b *Bypass) Capture(now time.Time) bool {
	if b.owed == 0 || now.Sub(b.touchAt) >= BypassWindow {
		b.owed = 0
		return false
	}
	b.owed--
	return true
}
