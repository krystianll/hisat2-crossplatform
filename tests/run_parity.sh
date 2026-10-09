#!/usr/bin/env bash
# Compare the C launchers (hisat2_launcher.c) with the upstream Perl/Python
# wrapper scripts on Linux/macOS. Build the six binaries first:
#   make hisat2-align-s hisat2-align-l hisat2-build-s hisat2-build-l hisat2-inspect-s hisat2-inspect-l
# usage: tests/run_parity.sh [WORK_DIR] [case-substring]
# Needs perl, python3, a C compiler (CC, default cc), zlib, libbz2, gzip, bzip2.
set -eu
H=$(cd "$(dirname "$0")/.." && pwd)
W=${1:-$(mktemp -d)}
ONLY=${2:-}
BINS="hisat2-align-s hisat2-align-l hisat2-build-s hisat2-build-l hisat2-inspect-s hisat2-inspect-l"
for b in $BINS; do
	[ -x "$H/$b" ] || { echo "missing $H/$b - build it first (make $BINS)"; exit 2; }
done
mkdir -p "$W"
W=$(cd "$W" && pwd)
rm -rf "$W/ref" "$W/launch" "$W/td" "$W/work"
mkdir -p "$W/ref" "$W/launch"

# Reference: the upstream scripts next to the binaries. The read-statistics
# script must be executable for the Perl wrapper to run its read-length check
# (as in a normal install); the Python wrappers get an explicit python3.
for b in $BINS; do cp "$H/$b" "$W/ref/"; cp "$H/$b" "$W/launch/"; done
cp "$H/hisat2" "$H/hisat2-build" "$H/hisat2-inspect" "$H/hisat2_read_statistics.py" "$W/ref/"
sed -i.orig '1s|.*|#!/usr/bin/env python3|' "$W/ref/hisat2-build" "$W/ref/hisat2-inspect"
rm -f "$W/ref/"*.orig
chmod +x "$W/ref/"*

for t in ALIGN:hisat2 BUILD:hisat2-build INSPECT:hisat2-inspect; do
	"${CC:-cc}" -O2 -Wall -D_DEFAULT_SOURCE "-DLAUNCH_${t%%:*}" -o "$W/launch/${t#*:}" \
		"$H/hisat2_launcher.c" -lbz2 -lz
done

python3 "$H/tests/gen_td.py" "$W/td"
"$W/launch/hisat2-build-s" -q "$W/td/ref.fa" "$W/td/idx"
"$W/launch/hisat2-build-l" -q "$W/td/ref.fa" "$W/td/lidx"
python3 "$H/tests/parity.py" "$W/ref" "$W/launch" "$W/td" "$W/work" "$ONLY"
