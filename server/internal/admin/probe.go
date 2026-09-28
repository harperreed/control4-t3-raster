// ABOUTME: Probe asks a running tt7-server's admin API whether it answers: the container healthcheck
// ABOUTME: (tt7-server -healthcheck), so the image needs no curl.
package admin

import (
	"context"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"net/netip"
	"time"
)

// Probe GETs /api/screens from the admin API listening on listen (as in screens.toml), with the
// admin token when there is one. It fails unless the reply is 200 with a JSON screen list.
func Probe(listen, token string, timeout time.Duration) error {
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, "GET", "http://"+probeAddr(listen)+"/api/screens", nil)
	if err != nil {
		return err
	}
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("GET /api/screens: HTTP %d", resp.StatusCode)
	}
	var doc struct {
		Screens []json.RawMessage `json:"screens"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&doc); err != nil {
		return fmt.Errorf("GET /api/screens: %w", err)
	}
	return nil
}

// probeAddr turns a listen address into one to dial: a wildcard host (0.0.0.0 or ::) becomes 127.0.0.1.
func probeAddr(listen string) string {
	host, port, err := net.SplitHostPort(listen)
	if err != nil {
		return listen
	}
	if ip, err := netip.ParseAddr(host); err == nil && ip.IsUnspecified() {
		host = "127.0.0.1"
	}
	return net.JoinHostPort(host, port)
}
