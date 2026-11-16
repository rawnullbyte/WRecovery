#!/bin/sh
# fetch-tools.sh - put xorriso + grub into ./.cache/tools without root
#
# Only needed if `xorriso` and `grub-mkimage` are not installed system-wide.
# build.sh looks for them in ./.cache/tools/usr/bin automatically.
#
# The packages are taken from the Arch Linux mirrors and unpacked into a
# private prefix. This is a convenience for rootless build hosts; the normal
# way is `sudo pacman -S xorriso grub` (Arch) or
# `sudo apt install xorriso grub-pc-bin grub-efi-amd64-bin` (Debian/Ubuntu).

set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEST=$ROOT/.cache/tools
MIRROR=https://geo.mirror.pkgbuild.com

msg() { printf '==> %s\n' "$*"; }

mkdir -p "$DEST"

fetch_pkg() { # repo name
	repo=$1
	name=$2
	json=$(curl -fsSL "https://archlinux.org/packages/$repo/x86_64/$name/json/") || return 1
	fname=$(printf '%s' "$json" | sed -n 's/.*"filename": "\([^"]*\)".*/\1/p')
	[ -n "$fname" ] || return 1
	msg "fetching $fname"
	curl -fsSL -o "$ROOT/.cache/$fname" "$MIRROR/$repo/os/x86_64/$fname"
	bsdtar xf "$ROOT/.cache/$fname" -C "$DEST"
	rm -f "$ROOT/.cache/$fname"
}

need() {
	if [ -x "$DEST/usr/bin/$1" ]; then
		return 0
	fi
	return 1
}

if need xorriso; then
	msg "xorriso already unpacked"
else
	fetch_pkg extra libisoburn
	fetch_pkg extra libisofs
	fetch_pkg extra libburn
fi

if need grub-mkimage; then
	msg "grub already unpacked"
else
	fetch_pkg core grub
fi

msg "tools are in $DEST/usr/bin"
LD_LIBRARY_PATH=$DEST/usr/lib "$DEST/usr/bin/xorriso" --version 2>/dev/null | head -1
LD_LIBRARY_PATH=$DEST/usr/lib "$DEST/usr/bin/grub-mkimage" --version 2>/dev/null | head -1