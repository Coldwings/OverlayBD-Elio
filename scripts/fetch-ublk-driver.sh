#!/usr/bin/env bash
# Fetch only the CI driver's C source from a kernel.org release archive.
set -euo pipefail

if [[ $# != 2 || ! $1 =~ ^[1-9][0-9]*\.[0-9]+$ ]]; then
    echo "usage: $0 MAJOR.MINOR OUTPUT_BASE" >&2
    exit 2
fi
release=$1
output_dir=$2/linux-$release
mkdir -p -- "$output_dir"
if [[ -s $output_dir/ublk_drv.c && -s $output_dir/ublk_drv.c.sha256 ]]; then
    expected=$(cat "$output_dir/ublk_drv.c.sha256")
    if [[ $expected =~ ^[[:xdigit:]]{64}$ ]] &&
        (cd "$output_dir" && printf '%s  ublk_drv.c\n' "$expected" | sha256sum -c -); then
        echo "Using cached Linux $release ublk driver source"
        exit 0
    fi
fi

scratch=$(mktemp -d "$output_dir/.fetch.XXXXXX")
cleanup() {
    rm -f -- "$scratch/archive.tar.xz" "$scratch/checksums.asc" \
        "$scratch/archive.sha256" "$scratch/ublk_drv.c" "$scratch/ublk_drv.c.sha256"
    rmdir -- "$scratch"
}
trap cleanup EXIT

download() {
    # Keep curl in the enclosing workflow timeout's process group so its
    # complete-helper deadline also terminates an active download.
    timeout --foreground --kill-after=5s 180s curl -fsSL \
        --retry 4 --retry-max-time 150 --connect-timeout 15 --max-time 60 \
        "$1" -o "$2"
}
origin=https://cdn.kernel.org/pub/linux/kernel/v${release%%.*}.x
archive=linux-$release.tar.xz
echo "Fetching official Linux $release release source from $origin"
download "$origin/sha256sums.asc" "$scratch/checksums.asc"
awk -v name="$archive" '
    $2 == name && length($1) == 64 && $1 ~ /^[[:xdigit:]]+$/ { hash = $1; matches++ }
    END { if (matches != 1) exit 1; print hash }
' "$scratch/checksums.asc" > "$scratch/archive.sha256"
expected=$(cat "$scratch/archive.sha256")
download "$origin/$archive" "$scratch/archive.tar.xz"
(cd "$scratch" && printf '%s  archive.tar.xz\n' "$expected" | sha256sum -c -)
tar --extract --xz --to-stdout --file "$scratch/archive.tar.xz" \
    "linux-$release/drivers/block/ublk_drv.c" > "$scratch/ublk_drv.c"
test -s "$scratch/ublk_drv.c"
sha256sum < "$scratch/ublk_drv.c" | awk '{ print $1 }' > "$scratch/ublk_drv.c.sha256"
mv -f -- "$scratch/ublk_drv.c" "$output_dir/ublk_drv.c"
mv -f -- "$scratch/ublk_drv.c.sha256" "$output_dir/ublk_drv.c.sha256"
mv -f -- "$scratch/archive.sha256" "$output_dir/archive.sha256"
echo "Cached Linux $release ublk driver source (archive SHA-256 $expected)"
