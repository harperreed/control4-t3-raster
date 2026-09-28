// ABOUTME: Tests for writing a screen's new URL back into screens.toml: comments and every other line survive.
// ABOUTME: Also checks refusals leave the file untouched.
package config

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestSetURLKeepsEverythingElse(t *testing.T) {
	path := fixture(t, good)
	if err := SetURL(path, "wall", `https://example.com/new?q="x"&y=\1`); err != nil {
		t.Fatal(err)
	}
	b, _ := os.ReadFile(path)
	got := string(b)
	want := strings.Replace(good, `url = "https://example.com/b"`, `url = "https://example.com/new?q=\"x\"&y=\\1"`, 1)
	if got != want {
		t.Errorf("file after SetURL:\n%s\nwant:\n%s", got, want)
	}
	c, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	if c.Screens[1].URL != `https://example.com/new?q="x"&y=\1` || c.Screens[0].URL != "http://example.com/a" {
		t.Errorf("urls after reload: %q %q", c.Screens[0].URL, c.Screens[1].URL)
	}
}

func TestSetURLKeepsTrailingCommentAndIndent(t *testing.T) {
	text := `[[screen]]
name = "a"
host = "10.0.0.1"
token_file = "token-a"
  url='http://old/'   # the lobby page
[[screen]]
name = "b"
host = "10.0.0.2"
token_file = "token-b"
url = "http://other/"
`
	path := fixture(t, text)
	if err := SetURL(path, "a", "http://new/"); err != nil {
		t.Fatal(err)
	}
	b, _ := os.ReadFile(path)
	want := strings.Replace(text, `  url='http://old/'   # the lobby page`, `  url = "http://new/"   # the lobby page`, 1)
	if string(b) != want {
		t.Errorf("got:\n%s\nwant:\n%s", b, want)
	}
}

func TestSetURLKeepsFileMode(t *testing.T) {
	path := fixture(t, good)
	if err := os.Chmod(path, 0o640); err != nil {
		t.Fatal(err)
	}
	if err := SetURL(path, "tabletop", "http://new/"); err != nil {
		t.Fatal(err)
	}
	st, _ := os.Stat(path)
	if st.Mode().Perm() != 0o640 {
		t.Errorf("mode %o after rewrite, want 640", st.Mode().Perm())
	}
}

// In Docker the config's DIRECTORY is bind-mounted: rename(2) onto a single-file bind mount fails
// (EBUSY), so the temp file must live next to screens.toml, not in $TMPDIR. A read-only directory
// holding a writable screens.toml shows where the temp file goes: SetURL can only fail there.
func TestSetURLWritesItsTempFileNextToTheConfig(t *testing.T) {
	if os.Getuid() == 0 {
		t.Skip("root ignores directory permissions")
	}
	path := fixture(t, good)
	dir := filepath.Dir(path)
	if err := os.Chmod(dir, 0o555); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { os.Chmod(dir, 0o755) })
	err := SetURL(path, "wall", "http://new/")
	if err == nil || !strings.Contains(err.Error(), dir) || !strings.Contains(err.Error(), "mount the directory") {
		t.Fatalf("error %v, want one about creating a temp file in %s", err, dir)
	}
	b, _ := os.ReadFile(path)
	if string(b) != good {
		t.Errorf("screens.toml changed after a failed save:\n%s", b)
	}
}

func TestSetURLRefusals(t *testing.T) {
	multi := `[[screen]]
name = "a"
host = "10.0.0.1"
token_file = "token-a"
url = """
http://x/"""
`
	cases := []struct{ name, text, screen, url, want string }{
		{"unknown screen", good, "kitchen", "http://x/", `no screen named "kitchen"`},
		{"bad url", good, "wall", "javascript:alert(1)", `url "javascript:alert(1)"`},
		{"multi-line string", multi, "a", "http://y/", "one-line"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			path := fixture(t, tc.text)
			err := SetURL(path, tc.screen, tc.url)
			if err == nil || !strings.Contains(err.Error(), tc.want) {
				t.Fatalf("error %v, want %q", err, tc.want)
			}
			b, _ := os.ReadFile(path)
			if string(b) != tc.text {
				t.Errorf("the file changed after a refusal:\n%s", b)
			}
		})
	}
}
