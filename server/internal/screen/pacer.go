// ABOUTME: Frame pacing for one screen: never push the same pixels twice in a row, never faster than max_fps.
// ABOUTME: Pure logic over a caller-supplied clock, so it is unit-tested without sleeping.
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
// is positive, check again after it.
func (p *Pacer) Check(now time.Time, hash [32]byte) (send bool, wait time.Duration) {
	if p.sent && hash == p.lastHash {
		return false, 0
	}
	if p.pushed {
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
