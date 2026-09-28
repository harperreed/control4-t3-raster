// ABOUTME: Exponential reconnect backoff for talking to panels: 1 s, 2 s, 4 s ... up to a cap.
// ABOUTME: Not safe for concurrent use; each loop owns its own.
package backoff

import "time"

// Backoff hands out doubling delays between min and max.
type Backoff struct {
	min, max, next time.Duration
}

func New(min, max time.Duration) *Backoff {
	return &Backoff{min: min, max: max, next: min}
}

// Next returns the delay to wait now and doubles the one after it.
func (b *Backoff) Next() time.Duration {
	d := b.next
	b.next = min(b.next*2, b.max)
	return d
}

// Reset starts over from the minimum, after a success.
func (b *Backoff) Reset() {
	b.next = b.min
}
