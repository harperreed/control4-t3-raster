// ABOUTME: Tests for a screen's navigation loop: URL changes and reloads return at once, failed loads retry
// ABOUTME: with backoff, and frames reach the panel only while the configured URL is loaded. Chrome is a scripted page.
package screen

import (
	"bytes"
	"context"
	"errors"
	"image"
	"image/color"
	"image/png"
	"io"
	"log/slog"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/chromedp/cdproto/input"

	"github.com/harperreed/control4-t3-raster/server/internal/config"
)

// scriptedPage stands in for the Chrome tab. Each Navigate is reported on navs and returns whatever
// the test sends on replies (or the context's error, if the loop cancels it first). Capture returns
// what the test sends on shots.
type scriptedPage struct {
	navs     chan string
	replies  chan error
	shots    chan []byte
	mu       sync.Mutex
	captures int
}

func newScriptedPage() *scriptedPage {
	return &scriptedPage{navs: make(chan string, 16), replies: make(chan error), shots: make(chan []byte, 4)}
}

func (p *scriptedPage) Navigate(ctx context.Context, url string) error {
	p.navs <- url
	select {
	case err := <-p.replies:
		return err
	case <-ctx.Done():
		return ctx.Err()
	}
}

func (p *scriptedPage) Capture(ctx context.Context) ([]byte, error) {
	select {
	case b := <-p.shots:
		p.mu.Lock()
		p.captures++
		p.mu.Unlock()
		return b, nil
	case <-ctx.Done():
		return nil, ctx.Err()
	}
}

func (p *scriptedPage) Mouse(ctx context.Context, typ input.MouseType, x, y float64) error {
	return nil
}

func (p *scriptedPage) captureCount() int {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.captures
}

// nextNav waits for the loop's next navigation and returns its URL.
func (p *scriptedPage) nextNav(t *testing.T, within time.Duration) string {
	t.Helper()
	select {
	case u := <-p.navs:
		return u
	case <-time.After(within):
		t.Fatalf("no navigation within %v", within)
		return ""
	}
}

// noNav checks that the loop starts no navigation for d.
func (p *scriptedPage) noNav(t *testing.T, d time.Duration) {
	t.Helper()
	select {
	case u := <-p.navs:
		t.Fatalf("unexpected navigation to %s", u)
	case <-time.After(d):
	}
}

// solidPNG is a 1280x800 PNG of one colour, like a screencast frame.
func solidPNG(t *testing.T, c color.NRGBA) []byte {
	t.Helper()
	img := image.NewNRGBA(image.Rect(0, 0, logicalW, logicalH))
	for i := 0; i < len(img.Pix); i += 4 {
		img.Pix[i], img.Pix[i+1], img.Pix[i+2], img.Pix[i+3] = c.R, c.G, c.B, c.A
	}
	var buf bytes.Buffer
	if err := png.Encode(&buf, img); err != nil {
		t.Fatal(err)
	}
	return buf.Bytes()
}

// startNav makes a running screen on a scripted page, with retry delays from min to max.
func startNav(t *testing.T, min, max time.Duration) (*Screen, *scriptedPage) {
	t.Helper()
	cfg := config.Screen{Name: "den", Host: "10.0.0.9:80", Token: "t", URL: "http://configured/", Enabled: true, HeartbeatS: 60}
	s := New(cfg, 5, slog.New(slog.NewTextHandler(io.Discard, nil)))
	s.navRetryMin, s.navRetryMax = min, max
	p := newScriptedPage()
	s.Attach(p)
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { s.navLoop(ctx); close(done) }()
	t.Cleanup(func() { cancel(); <-done })
	return s, p
}

func urlStatus(s *Screen) string {
	st := s.Status()
	if st.URLStatus == nil {
		return "<nil>"
	}
	return *st.URLStatus
}

func waitStatus(t *testing.T, s *Screen, want string) {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for urlStatus(s) != want {
		if time.Now().After(deadline) {
			t.Fatalf("url_status %s, want %s", urlStatus(s), want)
		}
		time.Sleep(time.Millisecond)
	}
}

func heldCapture(s *Screen) []byte {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.capture
}

func TestSetURLReturnsWhileTheNavigationRuns(t *testing.T) {
	s, p := startNav(t, time.Hour, time.Hour)
	if u := p.nextNav(t, time.Second); u != "http://configured/" {
		t.Fatalf("first navigation to %s", u)
	}
	if urlStatus(s) != URLLoading {
		t.Errorf("url_status %s while loading", urlStatus(s))
	}
	p.replies <- nil
	waitStatus(t, s, URLOK)

	start := time.Now()
	s.SetURL("http://new/") // the page has not answered: SetURL must not wait for it
	if took := time.Since(start); took > 100*time.Millisecond {
		t.Errorf("SetURL took %v", took)
	}
	if st := s.Status(); st.URL != "http://new/" || *st.URLStatus != URLLoading {
		t.Errorf("status after SetURL: url %s, url_status %s", st.URL, *st.URLStatus)
	}
	if u := p.nextNav(t, time.Second); u != "http://new/" {
		t.Fatalf("navigated to %s", u)
	}
	p.replies <- nil
	waitStatus(t, s, URLOK)
}

func TestFailedLoadsRetryWithBackoffUntilOneWorks(t *testing.T) {
	s, p := startNav(t, 40*time.Millisecond, 100*time.Millisecond)
	p.nextNav(t, time.Second)
	last := time.Now() // before the reply, so every gap measured from here is a lower bound
	p.replies <- errors.New("page load error net::ERR_CONNECTION_REFUSED")
	waitStatus(t, s, URLFailed)
	st := s.Status()
	if st.URLError == nil || !strings.Contains(*st.URLError, "ERR_CONNECTION_REFUSED") {
		t.Errorf("url_error %v", st.URLError)
	}
	if st.LastError == nil || !strings.Contains(*st.LastError, "ERR_CONNECTION_REFUSED") {
		t.Errorf("last_error %v", st.LastError)
	}

	// Retries go to the configured URL after 40, 80, then 100 ms (the cap).
	for i, min := range []time.Duration{40, 80, 100} {
		if u := p.nextNav(t, time.Second); u != "http://configured/" {
			t.Fatalf("retry %d went to %s", i+1, u)
		}
		if gap := time.Since(last); gap < min*time.Millisecond {
			t.Errorf("retry %d came %v after the failure, want at least %v", i+1, gap, min*time.Millisecond)
		}
		last = time.Now()
		p.replies <- errors.New("HTTP 502 Bad Gateway")
		waitStatus(t, s, URLFailed)
	}
	p.nextNav(t, time.Second)
	p.replies <- nil
	waitStatus(t, s, URLOK)
	if st := s.Status(); st.URLError != nil {
		t.Errorf("url_error %v after a good load", *st.URLError)
	}

	// Backoff starts over after a success: the next failure retries after the minimum again.
	s.Reload()
	p.nextNav(t, time.Second)
	p.replies <- errors.New("down again")
	waitStatus(t, s, URLFailed)
	failed := time.Now()
	p.nextNav(t, time.Second)
	if gap := time.Since(failed); gap > 90*time.Millisecond {
		t.Errorf("first retry after a success came %v after the failure, want about 40 ms", gap)
	}
	p.replies <- nil
}

func TestSetURLDuringTheRetryWaitNavigatesAtOnce(t *testing.T) {
	s, p := startNav(t, time.Hour, time.Hour)
	p.nextNav(t, time.Second)
	p.replies <- errors.New("unreachable")
	waitStatus(t, s, URLFailed)
	s.SetURL("http://other/")
	if u := p.nextNav(t, time.Second); u != "http://other/" {
		t.Fatalf("navigated to %s", u)
	}
	p.replies <- nil
	waitStatus(t, s, URLOK)
	p.noNav(t, 50*time.Millisecond) // the old URL's retry is gone
}

func TestSetURLCancelsAHungNavigation(t *testing.T) {
	s, p := startNav(t, time.Hour, time.Hour)
	p.nextNav(t, time.Second) // never answered: the host swallows packets
	s.SetURL("http://other/")
	if u := p.nextNav(t, time.Second); u != "http://other/" {
		t.Fatalf("navigated to %s", u)
	}
	if st := s.Status(); *st.URLStatus != URLLoading || st.URLError != nil {
		t.Errorf("the cancelled navigation was reported: url_status %s, url_error %v", *st.URLStatus, st.URLError)
	}
	p.replies <- nil
	waitStatus(t, s, URLOK)
}

func TestReloadNavigatesToTheConfiguredURL(t *testing.T) {
	s, p := startNav(t, time.Hour, time.Hour)
	p.nextNav(t, time.Second)
	p.replies <- errors.New("unreachable") // the tab now shows Chrome's error page, not the configured URL
	waitStatus(t, s, URLFailed)
	if err := s.Reload(); err != nil {
		t.Fatal(err)
	}
	if u := p.nextNav(t, time.Second); u != "http://configured/" {
		t.Fatalf("reload went to %s", u)
	}
	p.replies <- nil
	waitStatus(t, s, URLOK)
}

func TestReloadOnADisabledScreenFails(t *testing.T) {
	s := New(config.Screen{Name: "den", Host: "10.0.0.9:80", URL: "http://x/", HeartbeatS: 60}, 5,
		slog.New(slog.NewTextHandler(io.Discard, nil)))
	if err := s.Reload(); err == nil {
		t.Error("Reload of a disabled screen succeeded")
	}
	if s.Status().URLStatus != nil {
		t.Errorf("a disabled screen reports url_status %s", *s.Status().URLStatus)
	}
}

func TestFramesReachThePanelOnlyWhileTheURLIsLoaded(t *testing.T) {
	s, p := startNav(t, time.Hour, time.Hour)
	red, green, blue := solidPNG(t, color.NRGBA{255, 0, 0, 255}), solidPNG(t, color.NRGBA{0, 255, 0, 255}),
		solidPNG(t, color.NRGBA{0, 0, 255, 255})
	grey := solidPNG(t, color.NRGBA{128, 128, 128, 255})

	p.nextNav(t, time.Second)
	s.OnFrame(grey) // painted while loading
	if heldCapture(s) != nil {
		t.Fatal("a frame painted while loading was kept")
	}
	p.shots <- red // the screenshot taken once the page loaded
	p.replies <- nil
	waitStatus(t, s, URLOK)
	for deadline := time.Now().Add(time.Second); !bytes.Equal(heldCapture(s), red); {
		if time.Now().After(deadline) {
			t.Fatal("the screenshot after the load did not become the capture")
		}
		time.Sleep(time.Millisecond)
	}
	s.OnFrame(green)
	if !bytes.Equal(heldCapture(s), green) {
		t.Fatal("a frame of the loaded page was not kept")
	}

	s.SetURL("http://unreachable/")
	p.nextNav(t, time.Second)
	p.replies <- errors.New("page load error net::ERR_NAME_NOT_RESOLVED")
	waitStatus(t, s, URLFailed)
	s.OnFrame(blue) // Chrome's error page
	if !bytes.Equal(heldCapture(s), green) {
		t.Fatal("a frame of Chrome's error page replaced the last good frame")
	}
}

func TestTheScreenshotDoesNotReplaceANewerScreencastFrame(t *testing.T) {
	s, p := startNav(t, time.Hour, time.Hour)
	red, green := solidPNG(t, color.NRGBA{255, 0, 0, 255}), solidPNG(t, color.NRGBA{0, 255, 0, 255})
	p.nextNav(t, time.Second)
	p.replies <- nil
	waitStatus(t, s, URLOK)
	s.OnFrame(green) // the screencast got here before the screenshot came back
	p.shots <- red
	for deadline := time.Now().Add(time.Second); p.captureCount() == 0; {
		if time.Now().After(deadline) {
			t.Fatal("no screenshot taken")
		}
		time.Sleep(time.Millisecond)
	}
	time.Sleep(20 * time.Millisecond)
	if !bytes.Equal(heldCapture(s), green) {
		t.Error("the older screenshot replaced the newer screencast frame")
	}
}

func TestAnErrorPageAfterALoadHoldsFramesAndRetries(t *testing.T) {
	s, p := startNav(t, time.Hour, time.Hour)
	red, blue := solidPNG(t, color.NRGBA{255, 0, 0, 255}), solidPNG(t, color.NRGBA{0, 0, 255, 255})
	p.nextNav(t, time.Second)
	p.replies <- nil
	waitStatus(t, s, URLOK)
	s.OnFrame(red)

	// The page reloads itself (or follows a link) while its server is down.
	s.OnErrorPage("http://configured/")
	if urlStatus(s) != URLFailed {
		t.Errorf("url_status %s after an error page", urlStatus(s))
	}
	s.OnFrame(blue)
	if !bytes.Equal(heldCapture(s), red) {
		t.Fatal("the error page's frame was kept")
	}
	if u := p.nextNav(t, time.Second); u != "http://configured/" {
		t.Fatalf("the retry went to %s", u)
	}
	p.replies <- errors.New("still down")
	waitStatus(t, s, URLFailed)

	// The loop's own failed load also commits an error page: that must not start another retry at once.
	s.OnErrorPage("http://configured/")
	p.noNav(t, 50*time.Millisecond)
}
