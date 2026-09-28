#!/usr/bin/env bash
# ABOUTME: Rebuild the Inter font subsets that tt7d's fallback clock embeds (third_party/fonts/inter/*.ttf).
# ABOUTME: Usage: tools/subset-fonts.sh. Needs network once (the pinned Inter release) and uv (pinned fonttools).
#
# Run it only to change the character set or the font version; the build
# itself uses the committed subsets and never needs fonttools. After a run,
# update the sha256 lines in third_party/fonts/inter/PROVENANCE.
#
# Why subsets: each full static Inter TTF is about 410 KB; the clock needs
# printable ASCII plus the middle dot and the ellipsis, which fit in a few
# tens of KB. Hinting is dropped (stb_truetype ignores it). All name records
# are kept, so the copyright and OFL notice stay inside each font file.

set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
dest="$root/third_party/fonts/inter"
zip="$root/third_party/src/Inter-4.1.zip"
fonttools="fonttools==4.60.1"
# Printable ASCII, U+00B7 MIDDLE DOT, U+2026 HORIZONTAL ELLIPSIS.
unicodes="U+0020-007E,U+00B7,U+2026"
# Which upstream files, from the release zip's extras/ttf/.
fonts=(InterDisplay-Light Inter-Regular)

"$root/scripts/fetch-sources.sh" inter
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$dest"

unzip -q -o "$zip" LICENSE.txt -d "$work"
cp "$work/LICENSE.txt" "$dest/LICENSE.txt"
for name in "${fonts[@]}"; do
  unzip -q -o "$zip" "extras/ttf/$name.ttf" -d "$work"
  echo "subset-fonts: $name.ttf upstream sha256 $(sha256sum "$work/extras/ttf/$name.ttf" | cut -d' ' -f1)"
  uvx --quiet --from "$fonttools" pyftsubset "$work/extras/ttf/$name.ttf" \
    --unicodes="$unicodes" --layout-features=kern --no-hinting --name-IDs='*' \
    --output-file="$dest/$name.ttf"
done
(cd "$dest" && sha256sum LICENSE.txt "${fonts[@]/%/.ttf}" && wc -c "${fonts[@]/%/.ttf}")
