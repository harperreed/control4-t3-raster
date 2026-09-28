// ABOUTME: Writes one screen's new URL back into screens.toml by rewriting only that screen's `url =` line.
// ABOUTME: Comments and all other lines survive; the result is re-validated, then swapped in with rename.
package config

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strings"
)

// BurntSushi/toml (like the other Go TOML libraries) drops comments when it
// re-encodes a document, so the change is a text edit of one line instead.
// The edited text is parsed again and must give the same config with only
// that URL changed before it replaces the file.

var (
	screenHeaderRE = regexp.MustCompile(`^\s*\[\[\s*screen\s*\]\]\s*(#.*)?$`)
	tableHeaderRE  = regexp.MustCompile(`^\s*\[`)
	urlLineRE      = regexp.MustCompile(`^(\s*)url\s*=\s*`)
)

// SetURL sets screen name's url in the config file at path, atomically.
func SetURL(path, name, newURL string) error {
	if err := CheckURL(newURL); err != nil {
		return err
	}
	b, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	text := string(b)
	before, err := parse(path, text)
	if err != nil {
		return err
	}
	index := -1
	for i, s := range before.Screens {
		if s.Name == name {
			index = i
		}
	}
	if index < 0 {
		return fmt.Errorf("no screen named %q in %s", name, path)
	}

	lines := strings.SplitAfter(text, "\n")
	line, err := findURLLine(lines, index)
	if err != nil {
		return fmt.Errorf("%s: screen %q: %w", path, name, err)
	}
	if lines[line], err = rewriteURLLine(lines[line], newURL); err != nil {
		return fmt.Errorf("%s: screen %q: %w", path, name, err)
	}
	edited := strings.Join(lines, "")

	after, err := parse(path, edited)
	if err != nil {
		return fmt.Errorf("rewriting %s would break it (%v); nothing written", path, err)
	}
	before.Screens[index].URL = newURL
	if fmt.Sprintf("%+v", before) != fmt.Sprintf("%+v", after) {
		return fmt.Errorf("rewriting %s changed more than the url of %q; nothing written", path, name)
	}
	return writeAtomic(path, []byte(edited))
}

// findURLLine returns the line index of the url key inside the index-th [[screen]] table.
func findURLLine(lines []string, index int) (int, error) {
	seen, inTable := -1, false
	for i, l := range lines {
		switch {
		case screenHeaderRE.MatchString(l):
			seen++
			inTable = seen == index
		case tableHeaderRE.MatchString(l):
			inTable = false
		case inTable && urlLineRE.MatchString(l):
			return i, nil
		}
	}
	return 0, fmt.Errorf("cannot find its `url = ...` line to rewrite")
}

// rewriteURLLine replaces the string value on a `url = "..."` line and keeps its indent and trailing comment.
func rewriteURLLine(line, newURL string) (string, error) {
	m := urlLineRE.FindStringSubmatchIndex(line)
	indent, rest := line[m[2]:m[3]], line[m[1]:]
	oneLine := fmt.Errorf("its url must be a one-line \"...\" or '...' string to be rewritten")
	if strings.HasPrefix(rest, `"""`) || strings.HasPrefix(rest, "'''") || rest == "" {
		return "", oneLine
	}
	end := -1
	switch rest[0] {
	case '"':
		for i := 1; i < len(rest); i++ {
			if rest[i] == '\\' {
				i++
			} else if rest[i] == '"' {
				end = i
				break
			}
		}
	case '\'':
		if j := strings.IndexByte(rest[1:], '\''); j >= 0 {
			end = j + 1
		}
	}
	if end < 0 {
		return "", oneLine
	}
	return indent + "url = " + quote(newURL) + rest[end+1:], nil
}

// quote makes a TOML basic string. CheckURL already refused control characters.
func quote(s string) string {
	return `"` + strings.NewReplacer(`\`, `\\`, `"`, `\"`).Replace(s) + `"`
}

// writeAtomic replaces path with data through a temp file in the same directory, keeping the file's mode.
func writeAtomic(path string, data []byte) error {
	st, err := os.Stat(path)
	if err != nil {
		return err
	}
	tmp, err := os.CreateTemp(filepath.Dir(path), "."+filepath.Base(path)+".*")
	if err != nil {
		return err
	}
	defer os.Remove(tmp.Name()) // a no-op once the rename happened
	if _, err := tmp.Write(data); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Chmod(st.Mode().Perm()); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Sync(); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	if err := os.Rename(tmp.Name(), path); err != nil {
		return err
	}
	if d, err := os.Open(filepath.Dir(path)); err == nil {
		d.Sync()
		d.Close()
	}
	return nil
}
