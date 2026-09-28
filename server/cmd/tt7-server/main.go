// ABOUTME: tt7-server: drives N tt7 panels from one headless Chrome, one tab (one URL) per screen.
// ABOUTME: Reads screens.toml, pushes frames, relays touches as clicks, and serves the admin API.
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/signal"
	"sync"
	"syscall"
	"time"

	"github.com/harperreed/control4-t3-raster/server/internal/admin"
	"github.com/harperreed/control4-t3-raster/server/internal/browser"
	"github.com/harperreed/control4-t3-raster/server/internal/config"
	"github.com/harperreed/control4-t3-raster/server/internal/screen"
)

func main() {
	configPath := flag.String("config", "screens.toml", "the screens.toml to run (server/screens.example.toml documents it)")
	debug := flag.Bool("debug", false, "also log every pushed frame")
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "usage: tt7-server [-config screens.toml] [-debug]\n\n"+
			"Shows each [[screen]]'s url on its tt7 panel and turns touches into clicks.\n\n")
		flag.PrintDefaults()
	}
	flag.Parse()
	level := slog.LevelInfo
	if *debug {
		level = slog.LevelDebug
	}
	log := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: level}))
	if err := run(*configPath, log); err != nil {
		log.Error("tt7-server stopped", "err", err)
		os.Exit(1)
	}
}

func run(configPath string, log *slog.Logger) error {
	cfg, err := config.Load(configPath)
	if err != nil {
		return err
	}
	// Bind first: a busy port should fail before Chrome starts.
	ln, err := net.Listen("tcp", cfg.Listen)
	if err != nil {
		return fmt.Errorf("admin listen: %w", err)
	}
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	br, err := browser.Start(cfg.ChromePath, cfg.ChromeFlags)
	if err != nil {
		ln.Close()
		return err
	}
	defer br.Close()

	var screens []*screen.Screen
	var tabs []*browser.Tab
	for _, sc := range cfg.Screens {
		s := screen.New(sc, cfg.MaxFPS, log)
		screens = append(screens, s)
		if !sc.Enabled {
			continue
		}
		tab, err := br.OpenTab(s.OnFrame)
		if err != nil {
			return fmt.Errorf("screen %q: %w", sc.Name, err)
		}
		s.Attach(tab)
		tabs = append(tabs, tab)
	}

	srv := &http.Server{Handler: admin.New(screens, cfg.Path, cfg.AdminToken).Handler(), ReadHeaderTimeout: 10 * time.Second}
	go func() {
		if err := srv.Serve(ln); !errors.Is(err, http.ErrServerClosed) {
			log.Error("admin server", "err", err)
		}
	}()
	log.Info("tt7-server running", "admin", "http://"+ln.Addr().String()+"/", "screens", len(screens),
		"enabled", len(tabs), "max_fps", cfg.MaxFPS, "config", cfg.Path)

	runCtx, cancel := context.WithCancel(ctx)
	var wg sync.WaitGroup
	for i, s := range screens {
		if cfg.Screens[i].Enabled {
			wg.Go(func() { s.Run(runCtx) })
		}
	}

	var chromeDied bool
	select {
	case <-ctx.Done():
		log.Info("shutting down")
	case <-br.Done():
		chromeDied = true
		log.Error("Chrome exited; stopping")
	}
	cancel() // closes every event stream and stops the loops
	shutdownCtx, done := context.WithTimeout(context.Background(), 5*time.Second)
	defer done()
	srv.Shutdown(shutdownCtx)
	wg.Wait()
	for _, t := range tabs {
		t.Close()
	}
	if chromeDied {
		return errors.New("Chrome exited")
	}
	return nil
}
