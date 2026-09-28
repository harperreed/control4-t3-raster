// ABOUTME: Tests for the reconnect backoff: doubles from its minimum, caps at its maximum, resets.
// ABOUTME: Pure arithmetic, no clocks.
package backoff

import (
	"testing"
	"time"
)

func TestDoublesAndCaps(t *testing.T) {
	b := New(time.Second, time.Minute)
	want := []time.Duration{1, 2, 4, 8, 16, 32, 60, 60}
	for i, w := range want {
		if got := b.Next(); got != w*time.Second {
			t.Errorf("step %d: %v, want %v", i, got, w*time.Second)
		}
	}
	b.Reset()
	if got := b.Next(); got != time.Second {
		t.Errorf("after Reset: %v, want 1s", got)
	}
}
