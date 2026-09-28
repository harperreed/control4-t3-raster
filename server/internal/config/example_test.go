// ABOUTME: Keeps server/screens.example.toml valid: loads it with a fake HOME holding its token files and Chrome.
// ABOUTME: The real ~/.config/tt7 tokens are never read.
package config

import (
	"os"
	"path/filepath"
	"testing"
)

func TestExampleConfigLoads(t *testing.T) {
	home := t.TempDir()
	t.Setenv("HOME", home)
	for _, f := range []string{".config/tt7/token", ".config/tt7/token-tt7-942093"} {
		p := filepath.Join(home, f)
		os.MkdirAll(filepath.Dir(p), 0o700)
		if err := os.WriteFile(p, []byte("tok\n"), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	chrome := filepath.Join(home, ".agent-browser/browsers/chrome-154.0.8037.57/chrome")
	os.MkdirAll(filepath.Dir(chrome), 0o700)
	if err := os.WriteFile(chrome, []byte("#!/bin/sh\n"), 0o700); err != nil {
		t.Fatal(err)
	}
	c, err := Load("../../screens.example.toml")
	if err != nil {
		t.Fatal(err)
	}
	if len(c.Screens) != 2 || c.Screens[0].Host != "192.168.23.197:80" || c.Screens[1].Name != "wall" {
		t.Errorf("screens %+v", c.Screens)
	}
	if c.ChromePath != chrome || c.Listen != "127.0.0.1:7788" {
		t.Errorf("chrome %q listen %q", c.ChromePath, c.Listen)
	}
}
