// ABOUTME: Tests for Probe, the container healthcheck: it asks a running admin API for /api/screens with the
// ABOUTME: admin token, against a real admin handler on a real socket.
package admin

import (
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func TestProbeHealthyWithTheToken(t *testing.T) {
	srv, _ := setup(t, "s3cret")
	if err := Probe(srv.Listener.Addr().String(), "s3cret", time.Second); err != nil {
		t.Fatal(err)
	}
}

func TestProbeWithoutATokenOnLoopback(t *testing.T) {
	srv, _ := setup(t, "")
	if err := Probe(srv.Listener.Addr().String(), "", time.Second); err != nil {
		t.Fatal(err)
	}
}

func TestProbeFailsOnAWrongToken(t *testing.T) {
	srv, _ := setup(t, "s3cret")
	err := Probe(srv.Listener.Addr().String(), "wrong", time.Second)
	if err == nil || !strings.Contains(err.Error(), "401") {
		t.Fatalf("error %v, want a 401", err)
	}
}

func TestProbeFailsWhenNothingListens(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	addr := ln.Addr().String()
	ln.Close()
	if err := Probe(addr, "", time.Second); err == nil {
		t.Fatal("Probe succeeded with nothing listening")
	}
}

func TestProbeFailsWhenTheServerHangs(t *testing.T) {
	block := make(chan struct{})
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { <-block }))
	defer srv.Close()
	defer close(block)
	start := time.Now()
	if err := Probe(srv.Listener.Addr().String(), "", 200*time.Millisecond); err == nil {
		t.Fatal("Probe succeeded against a hung server")
	}
	if took := time.Since(start); took > time.Second {
		t.Errorf("Probe took %v with a 200 ms timeout", took)
	}
}

func TestProbeAddress(t *testing.T) {
	cases := map[string]string{
		"0.0.0.0:7788":   "127.0.0.1:7788",
		"[::]:7788":      "127.0.0.1:7788",
		"127.0.0.1:9000": "127.0.0.1:9000",
		"192.168.1.5:80": "192.168.1.5:80",
		"localhost:7788": "localhost:7788",
		"[::1]:7788":     "[::1]:7788",
	}
	for listen, want := range cases {
		if got := probeAddr(listen); got != want {
			t.Errorf("probeAddr(%q) = %q, want %q", listen, got, want)
		}
	}
}
