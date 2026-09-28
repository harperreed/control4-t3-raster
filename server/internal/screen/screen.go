// ABOUTME: One screen: a Chrome tab's frames paced onto one tt7d panel, heartbeats, and the panel's
// ABOUTME: touches turned into clicks. Three loops per screen, each with its own backoff; no screen waits on another.
package screen

import (
	"bytes"
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"log/slog"
	"sync"
	"time"

	"github.com/chromedp/cdproto/input"

	"github.com/harperreed/control4-t3-raster/server/internal/backoff"
	"github.com/harperreed/control4-t3-raster/server/internal/config"
	"github.com/harperreed/control4-t3-raster/server/internal/panel"
)

// Page is the browser tab a screen shows (browser.Tab).
type Page interface {
	Navigate(ctx context.Context, url string) error
	Reload(ctx context.Context) error
	Mouse(ctx context.Context, typ input.MouseType, x, y float64) error
}

const minBackoff, maxBackoff = time.Second, time.Minute

// bootID keeps frame ids unique across server restarts.
var bootID = func() string {
	b := make([]byte, 4)
	rand.Read(b)
	return hex.EncodeToString(b)
}()

// Status is what the admin API reports for a screen.
type Status struct {
	Name            string     `json:"name"`
	Host            string     `json:"host"`
	URL             string     `json:"url"`
	Enabled         bool       `json:"enabled"`
	Reachable       bool       `json:"reachable"`
	EventsConnected bool       `json:"events_connected"`
	DeviceID        *string    `json:"device_id"`
	LastPushAt      *time.Time `json:"last_push_at"`
	LastFrameID     *string    `json:"last_frame_id"`
	FramesPushed    int64      `json:"frames_pushed"`
	LastError       *string    `json:"last_error"`
	LastErrorAt     *time.Time `json:"last_error_at"`
}

// Screen drives one panel.
type Screen struct {
	cfg   config.Screen
	panel *panel.Client
	page  Page // nil while the screen is disabled
	pacer *Pacer
	log   *slog.Logger

	wake   chan struct{} // a new capture is waiting
	repush chan struct{} // the panel came back: push the current frame even if unchanged

	mu       sync.Mutex
	capture  []byte // the newest frame from Chrome
	forget   bool   // set by Repush, consumed by the push loop
	shown    []byte // the last frame the panel accepted (preview.png)
	st       Status
	seq      uint64
	mapper   TouchMapper
	touchLog bool // the current touch was already logged as being on an old frame
}

// New prepares a screen. Attach a Page and call Run to drive it.
func New(cfg config.Screen, maxFPS int, log *slog.Logger) *Screen {
	return &Screen{
		cfg:    cfg,
		panel:  panel.New(cfg.Host, cfg.Token),
		pacer:  NewPacer(maxFPS),
		log:    log.With("screen", cfg.Name),
		wake:   make(chan struct{}, 1),
		repush: make(chan struct{}, 1),
		st:     Status{Name: cfg.Name, Host: cfg.Host, URL: cfg.URL, Enabled: cfg.Enabled},
	}
}

func (s *Screen) Name() string { return s.cfg.Name }

// Attach gives the screen its browser tab.
func (s *Screen) Attach(p Page) { s.page = p }

// OnFrame takes a PNG from the tab's screencast. It never blocks.
func (s *Screen) OnFrame(png []byte) {
	if w, h, ok := pngSize(png); !ok || w != logicalW || h != logicalH {
		s.log.Warn("dropping a capture that is not a 1280x800 PNG", "width", w, "height", h)
		return
	}
	s.mu.Lock()
	s.capture = png
	s.mu.Unlock()
	signal(s.wake)
}

// Repush makes the push loop send the current frame again, even if the panel should already show it.
func (s *Screen) Repush() {
	s.mu.Lock()
	s.forget = true
	s.mu.Unlock()
	signal(s.wake)
	signal(s.repush)
}

func signal(ch chan struct{}) {
	select {
	case ch <- struct{}{}:
	default:
	}
}

// Status returns a copy of the screen's state.
func (s *Screen) Status() Status {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.st
}

// Preview returns the last frame the panel accepted, or nil.
func (s *Screen) Preview() []byte {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.shown
}

// SetURL points the screen at a new page (navigating if the screen runs).
func (s *Screen) SetURL(ctx context.Context, url string) error {
	s.mu.Lock()
	s.st.URL = url
	s.mu.Unlock()
	if s.page == nil {
		return nil
	}
	return s.navigate(ctx, url)
}

// Reload reloads the screen's page.
func (s *Screen) Reload(ctx context.Context) error {
	if s.page == nil {
		return fmt.Errorf("screen %q is disabled", s.cfg.Name)
	}
	err := s.page.Reload(ctx)
	if err != nil {
		s.fail("reload", err)
	}
	return err
}

func (s *Screen) navigate(ctx context.Context, url string) error {
	s.log.Info("navigating", "url", url)
	err := s.page.Navigate(ctx, url)
	if err != nil {
		s.fail("navigate", err)
	}
	return err
}

// fail records an error for the admin API and logs it.
func (s *Screen) fail(what string, err error) {
	msg := what + ": " + err.Error()
	now := time.Now().UTC()
	s.mu.Lock()
	changed := s.st.LastError == nil || *s.st.LastError != msg
	s.st.LastError, s.st.LastErrorAt = &msg, &now
	s.mu.Unlock()
	if changed { // a dead panel fails the same way every retry; say it once
		s.log.Warn(what+" failed", "err", err)
	}
}

// Run drives the screen until ctx ends: loads its URL, then pushes, heartbeats and listens.
func (s *Screen) Run(ctx context.Context) {
	var wg sync.WaitGroup
	wg.Go(func() { s.pushLoop(ctx) })
	wg.Go(func() { s.heartbeatLoop(ctx) })
	wg.Go(func() { s.eventLoop(ctx) })
	s.navigate(ctx, s.Status().URL)
	wg.Wait()
}

// sleep waits d, or less if ctx ends or wake fires. It reports false when ctx ended.
func sleep(ctx context.Context, d time.Duration, wake <-chan struct{}) bool {
	t := time.NewTimer(d)
	defer t.Stop()
	select {
	case <-ctx.Done():
		return false
	case <-t.C:
	case <-wake:
	}
	return true
}

func (s *Screen) pushLoop(ctx context.Context) {
	bo := backoff.New(minBackoff, maxBackoff)
	for {
		select {
		case <-ctx.Done():
			return
		case <-s.wake:
		}
		for pending := true; pending; {
			pending = false
			s.mu.Lock()
			png, forget := s.capture, s.forget
			s.forget = false
			s.mu.Unlock()
			if png == nil {
				break
			}
			if forget {
				s.pacer.Forget()
			}
			hash := sha256.Sum256(png)
			send, wait := s.pacer.Check(time.Now(), hash)
			if !send {
				if wait > 0 { // too soon after the last push: wait, then take the newest capture
					if !sleep(ctx, wait, nil) {
						return
					}
					pending = true
				}
				continue
			}
			if err := s.push(ctx, png); err != nil {
				if ctx.Err() != nil {
					return
				}
				s.fail("push frame", err)
				s.setReachable(false)
				s.pacer.Forget() // the panel may or may not have it
				if !sleep(ctx, bo.Next(), s.repush) {
					return
				}
				pending = true
				continue
			}
			s.pacer.Sent(time.Now(), hash)
			bo.Reset()
		}
	}
}

func (s *Screen) push(ctx context.Context, png []byte) error {
	s.mu.Lock()
	s.seq++
	id := fmt.Sprintf("%s-%s-%d", s.cfg.Name, bootID, s.seq)
	s.mu.Unlock()
	if err := s.panel.PutFrame(ctx, png, id); err != nil {
		return err
	}
	now := time.Now().UTC()
	s.mu.Lock()
	s.shown = png
	s.st.LastPushAt, s.st.LastFrameID = &now, &id
	s.st.FramesPushed++
	wasDown := !s.st.Reachable
	s.st.Reachable = true
	s.mu.Unlock()
	if wasDown {
		s.log.Info("panel reachable", "host", s.cfg.Host)
	}
	s.log.Debug("frame pushed", "frame_id", id, "bytes", len(png))
	return nil
}

func (s *Screen) setReachable(ok bool) {
	s.mu.Lock()
	s.st.Reachable = ok
	s.mu.Unlock()
}

func (s *Screen) heartbeatLoop(ctx context.Context) {
	every := time.Duration(s.cfg.HeartbeatS) * time.Second
	for sleep(ctx, every, nil) {
		fallback, err := s.panel.Heartbeat(ctx)
		if err != nil {
			if ctx.Err() == nil {
				s.fail("heartbeat", err)
				s.setReachable(false)
			}
			continue
		}
		s.setReachable(true)
		if fallback { // a heartbeat never brings the frame back (SPEC §41.1); a frame does
			s.log.Info("panel shows its fallback clock; re-pushing the frame")
			s.Repush()
		}
	}
}

func (s *Screen) eventLoop(ctx context.Context) {
	bo := backoff.New(minBackoff, maxBackoff)
	for {
		err := s.listen(ctx, bo)
		if ctx.Err() != nil {
			return
		}
		s.fail("events", err)
		s.mu.Lock()
		s.st.EventsConnected = false
		s.mu.Unlock()
		if !sleep(ctx, bo.Next(), nil) {
			return
		}
	}
}

// listen reads one event stream connection until it fails.
func (s *Screen) listen(ctx context.Context, bo *backoff.Backoff) error {
	stream, err := s.panel.Events(ctx)
	if err != nil {
		return err
	}
	defer stream.Close()
	defer s.releaseMouse(ctx)
	for {
		ev, err := stream.Next(ctx)
		if err != nil {
			return err
		}
		switch ev.Type {
		case "hello":
			// A new connection: the panel may have restarted and lost the frame (frames are not
			// persisted), so push it again. tt7d skips the redraw if it still has it.
			bo.Reset()
			s.mu.Lock()
			s.st.EventsConnected = true
			if ev.DeviceID != "" {
				id := ev.DeviceID
				s.st.DeviceID = &id
			}
			s.mu.Unlock()
			s.log.Info("events connected", "device_id", ev.DeviceID, "panel_frame_id", deref(ev.FrameID))
			s.Repush()
		case "touch":
			s.touch(ctx, ev)
		case "button":
			s.log.Info("button", "button", ev.Button, "action", ev.Action, "code", ev.Code)
		}
	}
}

func (s *Screen) touch(ctx context.Context, ev panel.Event) {
	s.mu.Lock()
	latest := s.st.LastFrameID
	stale := latest == nil || ev.FrameID == nil || *ev.FrameID != *latest
	if ev.Action == "down" {
		s.touchLog = false
	}
	logIt := stale && !s.touchLog
	if logIt {
		s.touchLog = true
	}
	m, ok := s.mapper.Map(Touch{Action: ev.Action, Pointer: ev.Pointer, X: ev.X, Y: ev.Y})
	s.mu.Unlock()
	if logIt { // delivered anyway: the page is what it is now
		s.log.Info("touch on a frame that is not the latest pushed", "touch_frame_id", deref(ev.FrameID),
			"latest_frame_id", deref(latest), "action", ev.Action)
	}
	if ok && s.page != nil {
		if err := s.page.Mouse(ctx, m.Type, m.X, m.Y); err != nil {
			s.fail("mouse", err)
		}
	}
}

// releaseMouse lets go of a button held when the stream dropped mid-touch.
func (s *Screen) releaseMouse(ctx context.Context) {
	s.mu.Lock()
	m, ok := s.mapper.Release()
	s.mu.Unlock()
	if ok && s.page != nil && ctx.Err() == nil {
		s.page.Mouse(ctx, m.Type, m.X, m.Y)
	}
}

func deref(p *string) string {
	if p == nil {
		return "null"
	}
	return *p
}

// pngSize reads the width and height from a PNG's IHDR without decoding it.
func pngSize(b []byte) (w, h int, ok bool) {
	sig := []byte("\x89PNG\r\n\x1a\n")
	if len(b) < 24 || !bytes.Equal(b[:8], sig) || string(b[12:16]) != "IHDR" {
		return 0, 0, false
	}
	return int(binary.BigEndian.Uint32(b[16:20])), int(binary.BigEndian.Uint32(b[20:24])), true
}
