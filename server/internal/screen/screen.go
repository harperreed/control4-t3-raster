// ABOUTME: One screen: a Chrome tab's frames paced onto one tt7d panel (changed regions, or a full frame),
// ABOUTME: heartbeats, and the panel's touches turned into clicks. Three loops per screen; no screen waits on another.
package screen

import (
	"bytes"
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"image"
	"image/png"
	"log/slog"
	"net/http"
	"sync"
	"time"

	"github.com/chromedp/cdproto/input"

	"github.com/harperreed/control4-t3-raster/server/internal/backoff"
	"github.com/harperreed/control4-t3-raster/server/internal/config"
	"github.com/harperreed/control4-t3-raster/server/internal/panel"
	"github.com/harperreed/control4-t3-raster/server/internal/regions"
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

	// Frame traffic: every accepted update is a full frame (PUT) or regions (PATCH).
	BytesSent       int64    `json:"bytes_sent"` // request bodies the panel accepted
	FullFrames      int64    `json:"full_frames"`
	RegionFrames    int64    `json:"region_frames"`
	BaseMismatches  int64    `json:"base_mismatches"`   // PATCHes refused because the panel showed another frame
	LastUpdate      *string  `json:"last_update"`       // "full" or "regions"
	LastRegionCount int      `json:"last_region_count"` // regions in the last update (0 for a full frame)
	LastPushBytes   int      `json:"last_push_bytes"`
	LastPushMS      *float64 `json:"last_push_ms"` // the last accepted update's request, send to reply
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
	hurry  chan struct{} // an urgent capture (a touch's result) is waiting: stop waiting out max_fps

	// What the panel shows, as far as the push loop knows (only the push loop touches these).
	base    *image.NRGBA // nil: unknown, so the next update is a full frame
	baseID  string
	baseSHA string
	noPatch bool // the panel answered PATCH with 404/405/415: full frames until it reconnects

	mu       sync.Mutex
	capture  []byte // the newest frame from Chrome
	urgent   bool   // capture (or one it replaced) is a touch's result: skip the max_fps wait
	bypass   Bypass
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
		hurry:  make(chan struct{}, 1),
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
	s.urgent = s.urgent || s.bypass.Capture(time.Now())
	urgent := s.urgent
	s.mu.Unlock()
	signal(s.wake)
	if urgent {
		signal(s.hurry)
	}
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
			png, forget, urgent := s.capture, s.forget, s.urgent
			s.forget, s.urgent = false, false
			s.mu.Unlock()
			if png == nil {
				break
			}
			if forget {
				s.pacer.Forget()
				s.base, s.noPatch = nil, false // the panel restarted or covered the frame: resync in full
			}
			hash := sha256.Sum256(png)
			send, wait := s.pacer.Check(time.Now(), hash, urgent)
			if !send {
				if wait > 0 { // too soon after the last push: wait (or until a touch's result comes), then take the newest
					if !sleep(ctx, wait, s.hurry) {
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

func (s *Screen) nextID() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.seq++
	return fmt.Sprintf("%s-%s-%d", s.cfg.Name, bootID, s.seq)
}

// push sends a capture: as changed regions (PATCH) when the panel's frame is known and little changed,
// else in full (PUT). A refused PATCH (the panel shows something else, or cannot patch) falls back to a
// full frame at once. On an error the panel's frame is unknown, so the next update is a full frame.
func (s *Screen) push(ctx context.Context, pngBytes []byte) error {
	var cur *image.NRGBA
	if img, err := png.Decode(bytes.NewReader(pngBytes)); err == nil {
		cur = regions.ToNRGBA(img)
	} else {
		s.log.Warn("cannot decode a capture; sending it whole", "err", err)
	}
	if cur != nil && !s.noPatch {
		rects, full := regions.Plan(s.base, cur, s.cfg.RegionMaxFraction)
		if !full && len(rects) == 0 {
			return nil // new PNG bytes, the same pixels the panel shows
		}
		if !full {
			sent, err := s.pushRegions(ctx, pngBytes, cur, rects)
			if sent || err != nil {
				return err
			}
		}
	}
	id := s.nextID()
	start := time.Now()
	if err := s.panel.PutFrame(ctx, pngBytes, id); err != nil {
		s.base = nil
		return err
	}
	sum := sha256.Sum256(pngBytes)
	s.base, s.baseID, s.baseSHA = cur, id, hex.EncodeToString(sum[:])
	s.accepted(pngBytes, id, "full", 0, len(pngBytes), time.Since(start))
	return nil
}

// pushRegions PATCHes rects of cur onto the panel's frame. sent is false (with no error) when a full
// frame should go instead: the regions would not be smaller, or the panel refused them.
func (s *Screen) pushRegions(ctx context.Context, pngBytes []byte, cur *image.NRGBA, rects []image.Rectangle) (sent bool, err error) {
	rs := make([]regions.Region, len(rects))
	for i, r := range rects {
		p, err := regions.EncodePNG(cur, r)
		if err != nil {
			return false, nil
		}
		rs[i] = regions.Region{Rect: r, PNG: p}
	}
	body, err := regions.Encode(rs)
	if err != nil || len(body) >= len(pngBytes) {
		return false, nil
	}
	id, sha := s.nextID(), regions.FrameSHA(s.baseSHA, body)
	start := time.Now()
	err = s.panel.PatchFrame(ctx, body, s.baseID, id, sha)
	var apiErr *panel.APIError
	switch {
	case err == nil:
		s.base, s.baseID, s.baseSHA = cur, id, sha
		s.accepted(pngBytes, id, "regions", len(rects), len(body), time.Since(start))
		return true, nil
	case errors.As(err, &apiErr) && apiErr.Code == "base_mismatch":
		s.mu.Lock()
		s.st.BaseMismatches++
		s.mu.Unlock()
		s.log.Info("the panel shows another frame (restarted, fallback clock, or another sender); sending a full frame",
			"base_frame_id", s.baseID, "panel", apiErr.Body)
	case errors.As(err, &apiErr) && (apiErr.Status == http.StatusNotFound || apiErr.Status == http.StatusMethodNotAllowed ||
		apiErr.Status == http.StatusUnsupportedMediaType):
		s.noPatch = true
		s.log.Warn("the panel does not take PATCH /api/v1/frame (older tt7d?); full frames until it reconnects", "err", err)
	case errors.As(err, &apiErr):
		s.log.Warn("the panel refused a region update; sending a full frame", "err", err, "reply", apiErr.Body)
	default:
		s.base = nil // a network error: it may or may not have applied
		return false, err
	}
	s.base = nil
	return false, nil
}

// accepted records an update the panel took.
func (s *Screen) accepted(pngBytes []byte, id, via string, nRegions, n int, took time.Duration) {
	now := time.Now().UTC()
	ms := float64(took.Microseconds()) / 1000
	s.mu.Lock()
	s.shown = pngBytes
	s.st.LastPushAt, s.st.LastFrameID = &now, &id
	s.st.FramesPushed++
	s.st.BytesSent += int64(n)
	if via == "regions" {
		s.st.RegionFrames++
	} else {
		s.st.FullFrames++
	}
	s.st.LastUpdate, s.st.LastRegionCount, s.st.LastPushBytes, s.st.LastPushMS = &via, nRegions, n, &ms
	wasDown := !s.st.Reachable
	s.st.Reachable = true
	s.mu.Unlock()
	if wasDown {
		s.log.Info("panel reachable", "host", s.cfg.Host)
	}
	s.log.Debug("frame pushed", "frame_id", id, "via", via, "regions", nRegions, "bytes", n, "ms", ms)
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
	if ev.Action == "down" || ev.Action == "up" { // the next capture shows its result: send it at once
		s.bypass.Touch(time.Now())
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
