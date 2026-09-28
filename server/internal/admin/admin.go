// ABOUTME: The admin HTTP API and page: list screens, change a screen's URL (saved to screens.toml),
// ABOUTME: reload a page, and fetch the last pushed frame. A Bearer admin token guards /api when configured.
package admin

import (
	"crypto/subtle"
	_ "embed"
	"encoding/json"
	"io"
	"net/http"
	"strings"
	"sync"

	"github.com/harperreed/control4-t3-raster/server/internal/config"
	"github.com/harperreed/control4-t3-raster/server/internal/screen"
)

//go:embed index.html
var indexHTML []byte

// Admin serves the admin API for a set of screens.
type Admin struct {
	screens    []*screen.Screen
	configPath string
	token      string // "" means no token is required (loopback only; config enforces it)
	saveMu     sync.Mutex
}

func New(screens []*screen.Screen, configPath, token string) *Admin {
	return &Admin{screens: screens, configPath: configPath, token: token}
}

// Handler routes the admin endpoints.
func (a *Admin) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /{$}", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Header().Set("Content-Security-Policy", "default-src 'self'; img-src 'self' blob:; style-src 'unsafe-inline'; script-src 'unsafe-inline'; frame-ancestors 'none'")
		w.Write(indexHTML)
	})
	mux.HandleFunc("GET /api/screens", a.auth(a.list))
	mux.HandleFunc("PUT /api/screens/{name}/url", a.auth(a.setURL))
	mux.HandleFunc("POST /api/screens/{name}/reload", a.auth(a.reload))
	mux.HandleFunc("GET /api/screens/{name}/preview.png", a.auth(a.preview))
	mux.HandleFunc("/api/", a.auth(func(w http.ResponseWriter, r *http.Request) {
		writeError(w, http.StatusNotFound, "not_found", "no such endpoint")
	}))
	return mux
}

func (a *Admin) auth(h http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if a.token != "" {
			got, ok := strings.CutPrefix(r.Header.Get("Authorization"), "Bearer ")
			if !ok || subtle.ConstantTimeCompare([]byte(got), []byte(a.token)) != 1 {
				w.Header().Set("WWW-Authenticate", "Bearer")
				writeError(w, http.StatusUnauthorized, "unauthorized", "send Authorization: Bearer <admin token>")
				return
			}
		}
		h(w, r)
	}
}

func (a *Admin) find(w http.ResponseWriter, r *http.Request) *screen.Screen {
	name := r.PathValue("name")
	for _, s := range a.screens {
		if s.Name() == name {
			return s
		}
	}
	writeError(w, http.StatusNotFound, "no_such_screen", "no screen named "+name)
	return nil
}

func (a *Admin) list(w http.ResponseWriter, r *http.Request) {
	out := make([]screen.Status, 0, len(a.screens))
	for _, s := range a.screens {
		out = append(out, s.Status())
	}
	writeJSON(w, http.StatusOK, map[string]any{"screens": out})
}

// setURL saves the URL to screens.toml first, then navigates, so a restart keeps what the API reported.
func (a *Admin) setURL(w http.ResponseWriter, r *http.Request) {
	s := a.find(w, r)
	if s == nil {
		return
	}
	var body struct {
		URL string `json:"url"`
	}
	dec := json.NewDecoder(io.LimitReader(r.Body, 8192))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&body); err != nil {
		writeError(w, http.StatusBadRequest, "bad_request", `body must be JSON like {"url": "https://..."}: `+err.Error())
		return
	}
	if err := config.CheckURL(body.URL); err != nil {
		writeError(w, http.StatusBadRequest, "invalid_url", err.Error())
		return
	}
	a.saveMu.Lock()
	err := config.SetURL(a.configPath, s.Name(), body.URL)
	a.saveMu.Unlock()
	if err != nil {
		writeError(w, http.StatusInternalServerError, "save_failed", err.Error())
		return
	}
	// A page that fails to load is still the configured URL; the error shows in last_error.
	s.SetURL(r.Context(), body.URL)
	writeJSON(w, http.StatusOK, s.Status())
}

func (a *Admin) reload(w http.ResponseWriter, r *http.Request) {
	s := a.find(w, r)
	if s == nil {
		return
	}
	if err := s.Reload(r.Context()); err != nil {
		writeError(w, http.StatusConflict, "reload_failed", err.Error())
		return
	}
	writeJSON(w, http.StatusOK, s.Status())
}

func (a *Admin) preview(w http.ResponseWriter, r *http.Request) {
	s := a.find(w, r)
	if s == nil {
		return
	}
	png := s.Preview()
	if png == nil {
		writeError(w, http.StatusNotFound, "no_frame", "nothing pushed to this panel yet")
		return
	}
	w.Header().Set("Content-Type", "image/png")
	w.Header().Set("Cache-Control", "no-store")
	w.Write(png)
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	enc := json.NewEncoder(w)
	enc.SetIndent("", "  ")
	enc.Encode(v) // a write error means the client left; nothing to tell it
}

func writeError(w http.ResponseWriter, status int, code, msg string) {
	writeJSON(w, status, map[string]string{"error": code, "message": msg})
}
