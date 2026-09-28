// ABOUTME: Loads and strictly validates screens.toml: server settings plus one [[screen]] table per panel.
// ABOUTME: Token files must be private (no group/other access), like scripts/wifi-setup.sh's env files.
package config

import (
	"errors"
	"fmt"
	"net"
	"net/netip"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"

	"github.com/BurntSushi/toml"
)

const (
	DefaultListen     = "127.0.0.1:7788"
	DefaultMaxFPS     = 5
	DefaultHeartbeatS = 60
	// DefaultRegionMaxFraction: above this share of the screen changed, a full frame goes instead of regions.
	DefaultRegionMaxFraction = 0.5
)

// Config is screens.toml after validation. Paths are absolute and tokens are loaded.
type Config struct {
	Path           string // the file it came from; URL changes are written back here
	Listen         string
	ChromePath     string // "" lets chromedp look for a Chrome on PATH
	ChromeFlags    []string
	AdminTokenFile string
	AdminToken     string // "" when no admin token is configured
	MaxFPS         int
	Screens        []Screen
}

// Screen is one panel and the page it shows.
type Screen struct {
	Name       string
	Host       string // host:port, the port defaulted to 80
	TokenFile  string
	Token      string
	URL        string
	Enabled    bool
	HeartbeatS int
	// RegionMaxFraction is the largest share of the screen (0..1) sent as changed regions (PATCH);
	// more than that goes as a full frame. 0 sends full frames only.
	RegionMaxFraction float64
}

// fileConfig mirrors the TOML. Pointers tell "absent" from a zero value.
type fileConfig struct {
	Listen         string       `toml:"listen"`
	ChromePath     string       `toml:"chrome_path"`
	ChromeFlags    []string     `toml:"chrome_flags"`
	AdminTokenFile string       `toml:"admin_token_file"`
	MaxFPS         *int         `toml:"max_fps"`
	Screens        []fileScreen `toml:"screen"`
}

type fileScreen struct {
	Name       string `toml:"name"`
	Host       string `toml:"host"`
	TokenFile  string `toml:"token_file"`
	URL        string `toml:"url"`
	Enabled    *bool  `toml:"enabled"`
	HeartbeatS *int   `toml:"heartbeat_s"`
	// A float; TOML's integers 0 and 1 are accepted too.
	RegionMaxFraction any `toml:"region_max_fraction"`
}

// Screen names end up in admin URLs (/api/screens/{name}) and frame ids.
var nameRE = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$`)

// A DNS name: labels of letters, digits and hyphens.
var hostnameRE = regexp.MustCompile(`^[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?(\.[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*$`)

// Load reads and validates a screens.toml. The first problem found is returned.
func Load(path string) (*Config, error) {
	abs, err := filepath.Abs(path)
	if err != nil {
		return nil, err
	}
	text, err := os.ReadFile(abs)
	if err != nil {
		return nil, err
	}
	return parse(abs, string(text))
}

// parse validates the TOML text of the file at path (used for Load and to check a rewrite before it lands).
func parse(path, text string) (*Config, error) {
	var fc fileConfig
	md, err := toml.Decode(text, &fc)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	if und := md.Undecoded(); len(und) > 0 {
		return nil, fmt.Errorf("%s: unknown key %s", path, und[0])
	}
	dir := filepath.Dir(path)
	c := &Config{Path: path, Listen: fc.Listen, ChromeFlags: fc.ChromeFlags, MaxFPS: DefaultMaxFPS}
	if c.Listen == "" {
		c.Listen = DefaultListen
	}
	loopback, err := checkListen(c.Listen)
	if err != nil {
		return nil, err
	}
	if fc.MaxFPS != nil {
		c.MaxFPS = *fc.MaxFPS
	}
	if c.MaxFPS < 1 || c.MaxFPS > 30 {
		return nil, fmt.Errorf("max_fps %d: must be 1..30 frames per second", c.MaxFPS)
	}
	if fc.ChromePath != "" {
		c.ChromePath = resolve(dir, fc.ChromePath)
		st, err := os.Stat(c.ChromePath)
		if err != nil || st.IsDir() || st.Mode().Perm()&0o111 == 0 {
			return nil, fmt.Errorf("chrome_path %q: not an executable file", fc.ChromePath)
		}
	}
	for _, f := range c.ChromeFlags {
		if !strings.HasPrefix(f, "--") || len(f) < 3 || strings.ContainsAny(f, " \t\n") {
			return nil, fmt.Errorf("chrome_flags %q: each flag must look like --name or --name=value", f)
		}
	}
	if fc.AdminTokenFile != "" {
		c.AdminTokenFile = resolve(dir, fc.AdminTokenFile)
		if c.AdminToken, err = readToken(c.AdminTokenFile); err != nil {
			return nil, fmt.Errorf("admin_token_file: %w", err)
		}
	} else if !loopback {
		return nil, fmt.Errorf("listen %q is not a loopback address, so admin_token_file is required", c.Listen)
	}
	if len(fc.Screens) == 0 {
		return nil, errors.New("no [[screen]] tables: nothing to drive")
	}
	names := map[string]bool{}
	hosts := map[string]string{}
	for i, fs := range fc.Screens {
		s, err := checkScreen(dir, i, fs)
		if err != nil {
			return nil, err
		}
		if names[s.Name] {
			return nil, fmt.Errorf("duplicate screen name %q", s.Name)
		}
		names[s.Name] = true
		if other, ok := hosts[s.Host]; ok {
			return nil, fmt.Errorf("screens %q and %q both use host %s", other, s.Name, s.Host)
		}
		hosts[s.Host] = s.Name
		c.Screens = append(c.Screens, s)
	}
	return c, nil
}

func checkScreen(dir string, i int, fs fileScreen) (Screen, error) {
	s := Screen{Name: fs.Name, URL: fs.URL, Enabled: true, HeartbeatS: DefaultHeartbeatS,
		RegionMaxFraction: DefaultRegionMaxFraction}
	if s.Name == "" {
		return s, fmt.Errorf("screen %d: name is required", i+1)
	}
	if !nameRE.MatchString(s.Name) {
		return s, fmt.Errorf("screen %d: name %q: use 1-64 letters, digits, '-' or '_', starting with a letter or digit", i+1, s.Name)
	}
	where := fmt.Sprintf("screen %q", s.Name)
	if fs.Host == "" {
		return s, fmt.Errorf("%s: host is required", where)
	}
	host, err := normalizeHost(fs.Host)
	if err != nil {
		return s, fmt.Errorf("%s: %w", where, err)
	}
	s.Host = host
	if fs.URL == "" {
		return s, fmt.Errorf("%s: url is required", where)
	}
	if err := CheckURL(fs.URL); err != nil {
		return s, fmt.Errorf("%s: %w", where, err)
	}
	if fs.TokenFile == "" {
		return s, fmt.Errorf("%s: token_file is required", where)
	}
	s.TokenFile = resolve(dir, fs.TokenFile)
	if s.Token, err = readToken(s.TokenFile); err != nil {
		return s, fmt.Errorf("%s: token_file: %w", where, err)
	}
	if fs.Enabled != nil {
		s.Enabled = *fs.Enabled
	}
	if fs.HeartbeatS != nil {
		s.HeartbeatS = *fs.HeartbeatS
	}
	if s.HeartbeatS < 1 || s.HeartbeatS > 3600 {
		return s, fmt.Errorf("%s: heartbeat_s %d: must be 1..3600 seconds", where, s.HeartbeatS)
	}
	switch v := fs.RegionMaxFraction.(type) {
	case nil:
	case float64:
		s.RegionMaxFraction = v
	case int64:
		s.RegionMaxFraction = float64(v)
	default:
		return s, fmt.Errorf("%s: region_max_fraction %v: must be a number from 0 to 1", where, v)
	}
	if s.RegionMaxFraction < 0 || s.RegionMaxFraction > 1 {
		return s, fmt.Errorf("%s: region_max_fraction %v: must be 0..1 (0 sends full frames only)", where, s.RegionMaxFraction)
	}
	return s, nil
}

// CheckURL accepts an absolute http, https or file URL with no control characters.
func CheckURL(raw string) error {
	u, err := url.Parse(raw)
	bad := err != nil || strings.IndexFunc(raw, func(r rune) bool { return r < 0x20 || r == 0x7f }) >= 0
	if !bad {
		switch u.Scheme {
		case "http", "https":
			bad = u.Host == ""
		case "file":
			bad = u.Path == ""
		default:
			bad = true
		}
	}
	if bad {
		return fmt.Errorf("url %q: must be an absolute http://, https:// or file:// URL", raw)
	}
	return nil
}

// normalizeHost turns "IP", "IP:port", "[v6]:port" or "name[:port]" into host:port (port 80 by default).
func normalizeHost(raw string) (string, error) {
	bad := fmt.Errorf("host %q: want an IP address or hostname with an optional :port, e.g. 192.168.23.197 or 192.168.23.197:80", raw)
	h, port := raw, "80"
	if strings.HasPrefix(raw, "[") || strings.Count(raw, ":") == 1 {
		var err error
		if h, port, err = net.SplitHostPort(raw); err != nil {
			return "", bad
		}
	}
	if n, err := strconv.Atoi(port); err != nil || n < 1 || n > 65535 {
		return "", bad
	}
	if _, err := netip.ParseAddr(h); err != nil && !hostnameRE.MatchString(h) {
		return "", bad
	}
	return net.JoinHostPort(h, port), nil
}

// checkListen validates the admin address and says whether it is loopback only.
func checkListen(listen string) (bool, error) {
	h, port, err := net.SplitHostPort(listen)
	n, perr := strconv.Atoi(port)
	if err != nil || perr != nil || n < 1 || n > 65535 {
		return false, fmt.Errorf("listen %q: want host:port, e.g. %s", listen, DefaultListen)
	}
	if h == "localhost" {
		return true, nil
	}
	ip, err := netip.ParseAddr(h)
	if err != nil {
		return false, fmt.Errorf("listen %q: the host must be an IP address or localhost", listen)
	}
	return ip.IsLoopback(), nil
}

// resolve expands a leading ~/ and makes a relative path relative to the config file's directory.
func resolve(dir, p string) string {
	if strings.HasPrefix(p, "~/") {
		if home, err := os.UserHomeDir(); err == nil {
			p = filepath.Join(home, p[2:])
		}
	}
	if !filepath.IsAbs(p) {
		p = filepath.Join(dir, p)
	}
	return p
}

// readToken returns the file's first line, trimmed, refusing files others can read.
func readToken(path string) (string, error) {
	st, err := os.Stat(path)
	if err != nil {
		return "", err
	}
	if !st.Mode().IsRegular() {
		return "", fmt.Errorf("%s is not a regular file", path)
	}
	if perm := st.Mode().Perm(); perm&0o077 != 0 {
		return "", fmt.Errorf("%s is mode %o; group/others must have no access. Run: chmod 600 %s", path, perm, path)
	}
	b, err := os.ReadFile(path)
	if err != nil {
		return "", err
	}
	line, _, _ := strings.Cut(string(b), "\n")
	line = strings.TrimSpace(line)
	if line == "" {
		return "", fmt.Errorf("%s: the token (first line) is empty", path)
	}
	return line, nil
}
