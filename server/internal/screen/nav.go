// ABOUTME: A screen's navigation loop: loads the configured URL in the background, retries a failed load with
// ABOUTME: backoff (5 s to 5 min), and lets frames reach the panel only while that URL is loaded.
package screen

import (
	"context"
	"errors"
	"fmt"
	"time"

	"github.com/harperreed/control4-t3-raster/server/internal/backoff"
)

// url_status values in the admin API.
const (
	URLLoading = "loading" // a load of the configured URL is under way
	URLOK      = "ok"      // the configured URL loaded; its frames go to the panel
	URLFailed  = "failed"  // the last load failed; the panel keeps its last good frame and a retry is due
)

// Delays between retries of a failed load.
const navRetryMin, navRetryMax = 5 * time.Second, 5 * time.Minute

// SetURL makes url the screen's page. It returns at once; the load runs in the background and
// url_status reports how it went. A load already under way is abandoned.
func (s *Screen) SetURL(url string) {
	s.mu.Lock()
	s.st.URL = url
	s.mu.Unlock()
	if s.page != nil {
		s.kickNav()
	}
}

// Reload loads the configured URL again (whatever the tab shows now, e.g. Chrome's error page).
func (s *Screen) Reload() error {
	if s.page == nil {
		return fmt.Errorf("screen %q is disabled", s.cfg.Name)
	}
	s.kickNav()
	return nil
}

// kickNav starts a fresh load of the configured URL now, abandoning any load under way and
// any retry wait, with the retry backoff started over.
func (s *Screen) kickNav() {
	s.mu.Lock()
	s.navGen++
	s.navReset = true
	s.st.URLStatus = ptr(URLLoading)
	s.showing = false
	if s.navCancel != nil {
		s.navCancel()
	}
	s.mu.Unlock()
	signal(s.navKick)
}

// OnErrorPage is told when the tab commits Chrome's error page. A page that was showing
// fine and then failed a load of its own (a reload or a link while its server is down)
// is treated like a failed load: frames stop and a retry of the configured URL starts.
// While a load of ours is under way or has failed, the loop already knows.
func (s *Screen) OnErrorPage(url string) {
	s.mu.Lock()
	if s.st.URLStatus == nil || *s.st.URLStatus != URLOK {
		s.mu.Unlock()
		return
	}
	msg := "the page failed to load " + url + " (Chrome's error page)"
	s.st.URLStatus, s.st.URLError = ptr(URLFailed), &msg
	s.showing = false
	if s.navCancel != nil { // the screenshot after the load, if it is still being taken
		s.navCancel()
	}
	s.mu.Unlock()
	s.fail("page", errors.New(msg))
	signal(s.navKick)
}

// navLoop loads the configured URL at start and whenever it is kicked, retrying failures with backoff.
func (s *Screen) navLoop(ctx context.Context) {
	bo := backoff.New(s.navRetryMin, s.navRetryMax)
	var retry <-chan time.Time // set while a failed load waits for its retry
	s.kickNav()
	for {
		select {
		case <-ctx.Done():
			return
		case <-s.navKick:
		case <-retry:
		}
		retry = nil
		s.mu.Lock()
		gen, url := s.navGen, s.st.URL
		if s.navReset {
			bo.Reset()
			s.navReset = false
		}
		s.st.URLStatus = ptr(URLLoading)
		s.showing = false
		nctx, cancel := context.WithCancel(ctx)
		s.navCancel = cancel
		s.mu.Unlock()

		s.log.Info("navigating", "url", url)
		err := s.page.Navigate(nctx, url)
		if ctx.Err() != nil {
			cancel()
			return
		}
		s.mu.Lock()
		current := gen == s.navGen
		s.mu.Unlock()
		switch {
		case !current: // SetURL or Reload came in meanwhile; its kick is waiting
		case err != nil:
			wait := bo.Next()
			msg := err.Error()
			s.mu.Lock()
			s.st.URLStatus, s.st.URLError = ptr(URLFailed), &msg
			s.mu.Unlock()
			s.fail("navigate", fmt.Errorf("%s: %w (retry in %v)", url, err, wait))
			retry = time.After(wait)
		default:
			bo.Reset()
			s.loaded(nctx, gen)
		}
		cancel()
		s.mu.Lock()
		s.navCancel = nil
		s.mu.Unlock()
	}
}

// loaded opens the gate for frames and offers a screenshot of the page, in case Chrome
// painted it before the gate opened and does not paint again (a page that sits still).
func (s *Screen) loaded(ctx context.Context, gen uint64) {
	s.mu.Lock()
	if gen != s.navGen {
		s.mu.Unlock()
		return
	}
	s.st.URLStatus, s.st.URLError = ptr(URLOK), nil
	s.showing = true
	seq := s.frameSeq
	s.mu.Unlock()
	s.log.Info("page loaded")

	png, err := s.page.Capture(ctx)
	if err != nil {
		if ctx.Err() == nil {
			s.log.Warn("screenshot after the load failed; waiting for the next paint", "err", err)
		}
		return
	}
	s.take(png, &seq) // dropped if a screencast frame came in since, or another load started
}

func ptr[T any](v T) *T { return &v }
