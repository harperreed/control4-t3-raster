// ABOUTME: Client for one tt7d panel (tt7d/README.md): PUT and PATCH /api/v1/frame, POST /api/v1/heartbeat,
// ABOUTME: and the WS /api/v1/events stream (Bearer token in the Authorization header).
package panel

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"time"

	"github.com/coder/websocket"
)

// Timeouts per request. A panel that stops answering holds up only its own screen.
const (
	frameTimeout     = 10 * time.Second
	heartbeatTimeout = 5 * time.Second
	dialTimeout      = 5 * time.Second
)

// Client talks to the tt7d at host (host:port).
type Client struct {
	host, token string
	http        *http.Client
}

func New(host, token string) *Client {
	// tt7d answers one request per connection with Connection: close, so there is nothing to keep alive.
	tr := &http.Transport{DisableKeepAlives: true, ResponseHeaderTimeout: frameTimeout}
	return &Client{host: host, token: token, http: &http.Client{Transport: tr}}
}

// APIError is a non-2xx reply, with tt7d's stable error code when it sent one.
type APIError struct {
	Status int
	Code   string
	Body   string
}

func (e *APIError) Error() string {
	if e.Code != "" {
		return fmt.Sprintf("HTTP %d %s", e.Status, e.Code)
	}
	return fmt.Sprintf("HTTP %d: %.200s", e.Status, e.Body)
}

func (c *Client) do(ctx context.Context, timeout time.Duration, method, path string, body []byte, hdr map[string]string) ([]byte, error) {
	ctx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	// An explicit (maybe empty) body gives a Content-Length, which tt7d requires on POST and PUT.
	req, err := http.NewRequestWithContext(ctx, method, "http://"+c.host+path, bytes.NewReader(body))
	if err != nil {
		return nil, err
	}
	req.ContentLength = int64(len(body))
	req.Header.Set("Authorization", "Bearer "+c.token)
	for k, v := range hdr {
		req.Header.Set(k, v)
	}
	resp, err := c.http.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	reply, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if err != nil {
		return nil, err
	}
	if resp.StatusCode/100 != 2 {
		var e struct {
			Error string `json:"error"`
		}
		json.Unmarshal(reply, &e)
		return nil, &APIError{Status: resp.StatusCode, Code: e.Error, Body: string(reply)}
	}
	return reply, nil
}

// PutFrame shows a 1280x800 PNG on the panel under frameID. Not persisted: frames change often
// and tt7d writes persisted frames to flash (SPEC §12).
func (c *Client) PutFrame(ctx context.Context, png []byte, frameID string) error {
	sum := sha256.Sum256(png)
	_, err := c.do(ctx, frameTimeout, http.MethodPut, "/api/v1/frame", png, map[string]string{
		"Content-Type":   "image/png",
		"X-Frame-ID":     frameID,
		"X-Frame-SHA256": hex.EncodeToString(sum[:]),
		"X-Persist":      "false",
	})
	return err
}

// PatchFrame applies a region container (regions.Encode) to the frame baseID, making frameID, whose
// SHA-256 must come out as sha (regions.FrameSHA). tt7d applies all regions or none; it answers 409
// base_mismatch when baseID is not what it shows. Not persisted: tt7d refuses X-Persist on a PATCH.
func (c *Client) PatchFrame(ctx context.Context, body []byte, baseID, frameID, sha string) error {
	_, err := c.do(ctx, frameTimeout, http.MethodPatch, "/api/v1/frame", body, map[string]string{
		"Content-Type":    "application/x-tt7-regions",
		"X-Base-Frame-ID": baseID,
		"X-Frame-ID":      frameID,
		"X-Frame-SHA256":  sha,
	})
	return err
}

// Heartbeat restarts the panel's fallback timer and reports whether its fallback clock is showing.
func (c *Client) Heartbeat(ctx context.Context) (fallbackActive bool, err error) {
	reply, err := c.do(ctx, heartbeatTimeout, http.MethodPost, "/api/v1/heartbeat", []byte{}, nil)
	if err != nil {
		return false, err
	}
	var r struct {
		Fallback struct {
			Active bool `json:"active"`
		} `json:"fallback"`
	}
	if err := json.Unmarshal(reply, &r); err != nil {
		return false, fmt.Errorf("heartbeat reply: %w", err)
	}
	return r.Fallback.Active, nil
}

// Event is one message from the event stream. Fields not in a message stay zero.
type Event struct {
	Type     string  `json:"type"` // hello, touch, button, presence
	Action   string  `json:"action"`
	Pointer  int     `json:"pointer"`
	X        float64 `json:"x"`
	Y        float64 `json:"y"`
	FrameID  *string `json:"frame_id"`
	Button   string  `json:"button"`
	Code     int     `json:"code"`
	DeviceID string  `json:"device_id"` // hello only
}

// Stream is an open event stream.
type Stream struct {
	conn *websocket.Conn
}

// Events opens the event stream. The first event is tt7d's hello.
func (c *Client) Events(ctx context.Context) (*Stream, error) {
	dctx, cancel := context.WithTimeout(ctx, dialTimeout)
	defer cancel()
	conn, resp, err := websocket.Dial(dctx, "ws://"+c.host+"/api/v1/events", &websocket.DialOptions{
		HTTPHeader: http.Header{"Authorization": {"Bearer " + c.token}},
	})
	if err != nil {
		if resp != nil && resp.StatusCode != http.StatusSwitchingProtocols {
			return nil, &APIError{Status: resp.StatusCode}
		}
		return nil, err
	}
	return &Stream{conn: conn}, nil
}

// Next blocks for the next event. tt7d's pings are answered while it waits.
func (s *Stream) Next(ctx context.Context) (Event, error) {
	var ev Event
	typ, data, err := s.conn.Read(ctx)
	if err != nil {
		return ev, err
	}
	if typ != websocket.MessageText {
		return ev, fmt.Errorf("unexpected binary message")
	}
	if err := json.Unmarshal(data, &ev); err != nil {
		return ev, fmt.Errorf("event %.100q: %w", data, err)
	}
	return ev, nil
}

// Close ends the stream with a normal closure.
func (s *Stream) Close() {
	s.conn.Close(websocket.StatusNormalClosure, "")
}
