// ABOUTME: Tests for the admin API's auth and error paths, with real (unstarted, disabled) screens and a real
// ABOUTME: screens.toml on disk; the e2e test covers the paths that need Chrome and panels.
package admin

import (
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/harperreed/control4-t3-raster/server/internal/config"
	"github.com/harperreed/control4-t3-raster/server/internal/screen"
)

func setup(t *testing.T, token string) (*httptest.Server, string) {
	t.Helper()
	dir := t.TempDir()
	os.WriteFile(filepath.Join(dir, "tok"), []byte("panel-token\n"), 0o600)
	path := filepath.Join(dir, "screens.toml")
	os.WriteFile(path, []byte(`# kept
[[screen]]
name = "den"
host = "10.0.0.9"
token_file = "tok"
url = "http://old/"  # kept too
enabled = false
`), 0o600)
	cfg, err := config.Load(path)
	if err != nil {
		t.Fatal(err)
	}
	log := slog.New(slog.NewTextHandler(io.Discard, nil))
	screens := []*screen.Screen{screen.New(cfg.Screens[0], cfg.MaxFPS, log)}
	srv := httptest.NewServer(New(screens, path, token).Handler())
	t.Cleanup(srv.Close)
	return srv, path
}

func call(t *testing.T, method, url, token, body string) (int, map[string]any) {
	t.Helper()
	req, _ := http.NewRequest(method, url, strings.NewReader(body))
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	var doc map[string]any
	json.NewDecoder(resp.Body).Decode(&doc)
	return resp.StatusCode, doc
}

func TestTokenRequiredWhenConfigured(t *testing.T) {
	srv, _ := setup(t, "s3cret")
	for _, tok := range []string{"", "wrong"} {
		status, doc := call(t, "GET", srv.URL+"/api/screens", tok, "")
		if status != 401 || doc["error"] != "unauthorized" {
			t.Errorf("token %q: %d %v", tok, status, doc)
		}
	}
	if status, _ := call(t, "PUT", srv.URL+"/api/screens/den/url", "", `{"url":"http://x/"}`); status != 401 {
		t.Errorf("PUT url without a token: %d", status)
	}
	status, doc := call(t, "GET", srv.URL+"/api/screens", "s3cret", "")
	if status != 200 || len(doc["screens"].([]any)) != 1 {
		t.Errorf("with the token: %d %v", status, doc)
	}
	resp, err := http.Get(srv.URL + "/") // the page itself holds nothing secret; it asks for the token
	if err != nil || resp.StatusCode != 200 {
		t.Errorf("GET /: %v %v", resp, err)
	}
}

func TestNoTokenOnLoopback(t *testing.T) {
	srv, _ := setup(t, "")
	status, doc := call(t, "GET", srv.URL+"/api/screens", "", "")
	if status != 200 {
		t.Fatalf("%d %v", status, doc)
	}
	s := doc["screens"].([]any)[0].(map[string]any)
	if s["name"] != "den" || s["enabled"] != false || s["reachable"] != false || s["device_id"] != nil || s["last_push_at"] != nil {
		t.Errorf("status %v", s)
	}
}

func TestSetURLOnDisabledScreenSaves(t *testing.T) {
	srv, path := setup(t, "")
	status, doc := call(t, "PUT", srv.URL+"/api/screens/den/url", "", `{"url":"https://new.example/"}`)
	if status != 202 || doc["url"] != "https://new.example/" {
		t.Fatalf("%d %v", status, doc)
	}
	b, _ := os.ReadFile(path)
	if !strings.Contains(string(b), `url = "https://new.example/"  # kept too`) || !strings.HasPrefix(string(b), "# kept\n") {
		t.Errorf("screens.toml:\n%s", b)
	}
}

func TestErrors(t *testing.T) {
	srv, path := setup(t, "")
	before, _ := os.ReadFile(path)
	cases := []struct {
		method, path, body string
		status             int
		code               string
	}{
		{"PUT", "/api/screens/nope/url", `{"url":"http://x/"}`, 404, "no_such_screen"},
		{"PUT", "/api/screens/den/url", `{"url":"file"}`, 400, "invalid_url"},
		{"PUT", "/api/screens/den/url", `{"link":"http://x/"}`, 400, "bad_request"},
		{"PUT", "/api/screens/den/url", `not json`, 400, "bad_request"},
		{"POST", "/api/screens/den/reload", ``, 409, "reload_failed"}, // disabled: no page
		{"GET", "/api/screens/den/preview.png", ``, 404, "no_frame"},
		{"GET", "/api/nothing", ``, 404, "not_found"},
	}
	for _, c := range cases {
		status, doc := call(t, c.method, srv.URL+c.path, "", c.body)
		if status != c.status || doc["error"] != c.code {
			t.Errorf("%s %s %s: %d %v, want %d %s", c.method, c.path, c.body, status, doc, c.status, c.code)
		}
	}
	after, _ := os.ReadFile(path)
	if string(after) != string(before) {
		t.Errorf("refused requests changed screens.toml")
	}
}
