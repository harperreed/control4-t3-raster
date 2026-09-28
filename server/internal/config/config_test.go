// ABOUTME: Tests for screens.toml parsing and validation: defaults, every refusal, token file modes.
// ABOUTME: Real files in a temp dir; nothing mocked.
package config

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// fixture writes a token file (mode 600) and a screens.toml, returning the toml path.
func fixture(t *testing.T, toml string) string {
	t.Helper()
	dir := t.TempDir()
	for _, name := range []string{"token-a", "token-b", "admin-token"} {
		if err := os.WriteFile(filepath.Join(dir, name), []byte("secret-"+name+"\n"), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	path := filepath.Join(dir, "screens.toml")
	if err := os.WriteFile(path, []byte(toml), 0o644); err != nil {
		t.Fatal(err)
	}
	return path
}

const good = `
# the owner's panels
chrome_path = "/bin/sh"
chrome_flags = ["--no-sandbox"]

[[screen]]
name = "tabletop"
host = "192.168.23.197"
token_file = "token-a"   # relative to this file
url = "http://example.com/a"

[[screen]]
name = "wall"
host = "192.168.23.198:8080"
token_file = "token-b"
url = "https://example.com/b"
enabled = false
heartbeat_s = 30
`

func TestLoadGood(t *testing.T) {
	path := fixture(t, good)
	c, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	if c.Listen != "127.0.0.1:7788" {
		t.Errorf("listen default %q", c.Listen)
	}
	if c.MaxFPS != 5 {
		t.Errorf("max_fps default %d", c.MaxFPS)
	}
	if len(c.ChromeFlags) != 1 || c.ChromeFlags[0] != "--no-sandbox" {
		t.Errorf("chrome_flags %v", c.ChromeFlags)
	}
	if len(c.Screens) != 2 {
		t.Fatalf("screens %d", len(c.Screens))
	}
	a, b := c.Screens[0], c.Screens[1]
	if a.Name != "tabletop" || a.Host != "192.168.23.197:80" || a.URL != "http://example.com/a" {
		t.Errorf("screen a %+v", a)
	}
	if !a.Enabled || a.HeartbeatS != 60 {
		t.Errorf("screen a defaults: enabled %v heartbeat %d", a.Enabled, a.HeartbeatS)
	}
	if a.Token != "secret-token-a" {
		t.Errorf("token %q (first line, trimmed)", a.Token)
	}
	if a.TokenFile != filepath.Join(filepath.Dir(path), "token-a") {
		t.Errorf("token_file not resolved against the config dir: %q", a.TokenFile)
	}
	if b.Host != "192.168.23.198:8080" || b.Enabled || b.HeartbeatS != 30 {
		t.Errorf("screen b %+v", b)
	}
	if c.AdminToken != "" {
		t.Errorf("no admin token expected on loopback, got %q", c.AdminToken)
	}
}

func TestTildeExpandsToHome(t *testing.T) {
	home := t.TempDir()
	t.Setenv("HOME", home)
	if err := os.WriteFile(filepath.Join(home, "tok"), []byte("x"), 0o600); err != nil {
		t.Fatal(err)
	}
	path := fixture(t, `[[screen]]
name = "a"
host = "10.0.0.1"
token_file = "~/tok"
url = "http://x/"
`)
	c, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	if c.Screens[0].TokenFile != filepath.Join(home, "tok") {
		t.Errorf("token_file %q", c.Screens[0].TokenFile)
	}
}

func TestRefusals(t *testing.T) {
	screen := func(body string) string {
		return "[[screen]]\n" + body
	}
	ok := `name = "a"
host = "10.0.0.1"
token_file = "token-a"
url = "http://x/"
`
	cases := []struct {
		name, toml, want string
	}{
		{"no screens", `listen = "127.0.0.1:1"`, "no [[screen]]"},
		{"missing name", screen(strings.Replace(ok, `name = "a"`, "", 1)), `screen 1: name is required`},
		{"bad name", screen(strings.Replace(ok, `"a"`, `"has space"`, 1)), `name "has space"`},
		{"missing host", screen(strings.Replace(ok, `host = "10.0.0.1"`, "", 1)), `screen "a": host is required`},
		{"host with scheme", screen(strings.Replace(ok, `10.0.0.1`, `http://10.0.0.1`, 1)), `host "http://10.0.0.1"`},
		{"host with path", screen(strings.Replace(ok, `10.0.0.1`, `10.0.0.1/api`, 1)), `host "10.0.0.1/api"`},
		{"host bad port", screen(strings.Replace(ok, `10.0.0.1`, `10.0.0.1:99999`, 1)), `host "10.0.0.1:99999"`},
		{"missing url", screen(strings.Replace(ok, `url = "http://x/"`, "", 1)), `screen "a": url is required`},
		{"relative url", screen(strings.Replace(ok, `http://x/`, `/just/a/path`, 1)), `url "/just/a/path"`},
		{"ftp url", screen(strings.Replace(ok, `http://x/`, `ftp://x/`, 1)), `url "ftp://x/"`},
		{"missing token_file", screen(strings.Replace(ok, `token_file = "token-a"`, "", 1)), `screen "a": token_file is required`},
		{"token file absent", screen(strings.Replace(ok, `token-a`, `nope`, 1)), `nope`},
		{"heartbeat zero", screen(ok + "heartbeat_s = 0\n"), `heartbeat_s 0`},
		{"unknown key", screen(ok + "colour = \"red\"\n"), `unknown key screen.colour`},
		{"duplicate names", screen(ok) + screen(strings.Replace(ok, "10.0.0.1", "10.0.0.2", 1)), `duplicate screen name "a"`},
		{"duplicate hosts", screen(ok) + screen(strings.Replace(ok, `"a"`, `"b"`, 1)), `screens "a" and "b" both use host 10.0.0.1:80`},
		{"bad listen", "listen = \"nonsense\"\n" + screen(ok), `listen "nonsense"`},
		{"public listen without admin token", "listen = \"0.0.0.0:7788\"\n" + screen(ok), `admin_token_file is required`},
		{"max_fps too high", "max_fps = 100\n" + screen(ok), `max_fps 100`},
		{"chrome_path missing", "chrome_path = \"/no/such/chrome\"\n" + screen(ok), `chrome_path "/no/such/chrome"`},
		{"bad chrome flag", "chrome_flags = [\"no-dashes\"]\n" + screen(ok), `chrome_flags "no-dashes"`},
		{"toml syntax", "listen = \n", `screens.toml`},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			_, err := Load(fixture(t, tc.toml))
			if err == nil {
				t.Fatalf("accepted:\n%s", tc.toml)
			}
			if !strings.Contains(err.Error(), tc.want) {
				t.Errorf("error %q does not mention %q", err, tc.want)
			}
		})
	}
}

func TestTokenFileModeRefused(t *testing.T) {
	path := fixture(t, `[[screen]]
name = "a"
host = "10.0.0.1"
token_file = "token-a"
url = "http://x/"
`)
	tok := filepath.Join(filepath.Dir(path), "token-a")
	for _, mode := range []os.FileMode{0o644, 0o640, 0o604} {
		if err := os.Chmod(tok, mode); err != nil {
			t.Fatal(err)
		}
		_, err := Load(path)
		if err == nil || !strings.Contains(err.Error(), "chmod 600") {
			t.Errorf("mode %o: error %v, want a chmod 600 hint", mode, err)
		}
	}
	if err := os.Chmod(tok, 0o400); err != nil {
		t.Fatal(err)
	}
	if _, err := Load(path); err != nil {
		t.Errorf("mode 400 should be accepted: %v", err)
	}
}

func TestEmptyTokenRefused(t *testing.T) {
	path := fixture(t, `[[screen]]
name = "a"
host = "10.0.0.1"
token_file = "token-a"
url = "http://x/"
`)
	if err := os.WriteFile(filepath.Join(filepath.Dir(path), "token-a"), []byte("\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := Load(path); err == nil || !strings.Contains(err.Error(), "empty") {
		t.Errorf("error %v, want empty token refusal", err)
	}
}

func TestAdminTokenOnPublicListen(t *testing.T) {
	path := fixture(t, `listen = "0.0.0.0:7788"
admin_token_file = "admin-token"
[[screen]]
name = "a"
host = "10.0.0.1"
token_file = "token-a"
url = "http://x/"
`)
	c, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	if c.AdminToken != "secret-admin-token" {
		t.Errorf("admin token %q", c.AdminToken)
	}
}

func TestIPv6Host(t *testing.T) {
	path := fixture(t, `[[screen]]
name = "a"
host = "[fe80::1]:81"
token_file = "token-a"
url = "http://x/"
`)
	c, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	if c.Screens[0].Host != "[fe80::1]:81" {
		t.Errorf("host %q", c.Screens[0].Host)
	}
}
