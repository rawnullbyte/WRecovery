#!/bin/sh
# build.sh - build the WRecovery ISO
#
#   ./build.sh                 full build -> out/wrecovery.iso
#   ./build.sh --jobs 8        parallel build
#   ./build.sh --iso-only      reuse kernel/busybox from .build
#   ./build.sh --clean         wipe .build and start over
#   ./build.sh --size-check    fail if the ISO is larger than the limit
#
# Requirements (Arch):  gcc make bc flex bison cpio perl xz xorriso grub
#                       python3
# CI/apt:               build-essential bc bison flex libelf-dev libssl-dev
#                       xorriso grub-pc-bin grub-efi-amd64-bin python3
#
# Nothing is installed as root and nothing outside this directory is touched,
# except for the usual ccache-free build cache in ./.build and ./.cache.

set -eu

VERSION=0.3.0
KVER=6.18.63
BBVER=1.36.1
ISO_MAX_MB=10

KURL="https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$KVER.tar.xz"
BBURL="https://busybox.net/downloads/busybox-$BBVER.tar.bz2"
KSHA256=81764a545072d160748beff4d9de2e7d95aa9f16e8261110288f067a2cc61d71
BBSHA256=b8cc24c9574d809e7279c3be349795c5d5ceb6fdf19ca709f80cde50e47de314

ROOT=$(cd "$(dirname "$0")" && pwd)
CACHE=$ROOT/.cache
BUILD=$ROOT/.build
OUT=$ROOT/out
JOBS=$(nproc 2>/dev/null || echo 4)
ISO_ONLY=0
SIZE_CHECK=0

while [ $# -gt 0 ]; do
	case $1 in
	--jobs) JOBS=$2; shift 2 ;;
	--jobs=*) JOBS=${1#*=}; shift ;;
	--iso-only) ISO_ONLY=1; shift ;;
	--clean) rm -rf "$BUILD" "$OUT"; shift ;;
	--size-check) SIZE_CHECK=1; shift ;;
	-h|--help) sed -n '2,20p' "$0"; exit 0 ;;
	*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
done

msg()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m warn:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

mkdir -p "$CACHE" "$BUILD" "$OUT"

# ---------------------------------------------------------------- tool lookup --

# Tools may come from PATH, or from the self-contained .cache/tools prefix
# (populated by scripts/fetch-tools.sh on machines without root).
find_tool() { # name
	if command -v "$1" >/dev/null 2>&1; then
		command -v "$1"
		return 0
	fi
	if [ -x "$CACHE/tools/usr/bin/$1" ]; then
		printf '%s\n' "$CACHE/tools/usr/bin/$1"
		return 0
	fi
	return 1
}

tool_env() {
	if [ -d "$CACHE/tools/usr/lib" ]; then
		LD_LIBRARY_PATH="$CACHE/tools/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
		export LD_LIBRARY_PATH
	fi
}
tool_env

XORRISO=$(find_tool xorriso) || die "xorriso not found - install it (Arch: sudo pacman -S xorriso, Debian: sudo apt install xorriso)"
GRUB_MKIMAGE=$(find_tool grub-mkimage) || die "grub-mkimage not found - install it (Arch: sudo pacman -S grub, Debian: sudo apt install grub-pc-bin grub-efi-amd64-bin)"
PYTHON=$(find_tool python3) || die "python3 not found"

# grub module directory
for d in "$CACHE/tools/usr/lib/grub" /usr/lib/grub /usr/share/grub2; do
	if [ -d "$d/i386-pc" ] && [ -d "$d/x86_64-efi" ]; then
		GRUBDIR=$d
		break
	fi
done
[ -n "${GRUBDIR:-}" ] || die "grub modules (i386-pc + x86_64-efi) not found"

for t in gcc make ld bc flex bison cpio sed awk; do
	command -v "$t" >/dev/null 2>&1 || die "missing build tool: $t"
done

# ------------------------------------------------------------------- helpers --

fetch() { # url file sha256
	url=$1; file=$2; want=$3
	if [ -f "$CACHE/$file" ] && [ -n "$want" ]; then
		have=$(sha256sum "$CACHE/$file" | cut -d' ' -f1)
		if [ "$have" = "$want" ]; then
			return 0
		fi
		warn "$file: checksum mismatch, re-downloading"
		rm -f "$CACHE/$file"
	fi
	if [ ! -f "$CACHE/$file" ]; then
		msg "downloading $file"
		curl -fL --retry 3 -o "$CACHE/$file.part" "$url"
		mv "$CACHE/$file.part" "$CACHE/$file"
	fi
	if [ -n "$want" ]; then
		have=$(sha256sum "$CACHE/$file" | cut -d' ' -f1)
		[ "$have" = "$want" ] || die "$file: bad checksum (got $have)"
	fi
}

unpack() { # archive destdir
	archive=$1; dest=$2; marker=$3
	[ -d "$dest" ] && [ -e "$dest/$marker" ] && return 0
	msg "unpacking $(basename "$archive")"
	rm -rf "$dest"
	mkdir -p "$dest"
	case $archive in
	*.tar.xz)  tar xf "$archive" -C "$dest" --strip-components=1 ;;
	*.tar.bz2) tar xf "$archive" -C "$dest" --strip-components=1 ;;
	*) die "unknown archive type: $archive" ;;
	esac
}

# ------------------------------------------------------------------- winreg ---

build_winreg() {
	msg "building winreg and the menu"
	mkdir -p "$BUILD/bin"
	gcc -Os -static -s -Wall -Wextra -o "$BUILD/bin/winreg" "$ROOT/src/winreg.c"
	gcc -Os -static -s -Wall -Wextra -DWRE_VERSION='"'"$VERSION"'"' \
		-o "$BUILD/bin/wretui" "$ROOT/src/wretui.c"
	for b in winreg wretui; do
		ls -l "$BUILD/bin/$b" | awk '{printf "    %s: %d KiB\n", $9, $5/1024}'
	done
}

# ------------------------------------------------------------------ busybox ---

build_busybox() {
	if [ -x "$BUILD/busybox-$BBVER/busybox" ] && \
	   [ "$BUILD/busybox-$BBVER/busybox" -nt "$ROOT/src/busybox.config" ] 2>/dev/null; then
		return 0
	fi
	[ "$ISO_ONLY" = 1 ] && die "--iso-only requested but no busybox build found"

	fetch "$BBURL" "busybox-$BBVER.tar.bz2" "$BBSHA256"
	unpack "$CACHE/busybox-$BBVER.tar.bz2" "$BUILD/busybox-$BBVER" Makefile

	msg "configuring busybox"
	( cd "$BUILD/busybox-$BBVER" && make defconfig >/dev/null && cp "$ROOT/src/busybox.config" .config && make oldconfig >/dev/null )
	msg "building busybox (static)"
	( cd "$BUILD/busybox-$BBVER" && make -j"$JOBS" >"$BUILD/busybox.log" 2>&1 ) || {
		tail -20 "$BUILD/busybox.log" >&2
		die "busybox build failed (see .build/busybox.log)"
	}
	file "$BUILD/busybox-$BBVER/busybox" | grep -q "statically linked" || \
		die "busybox is not statically linked"
}

# -------------------------------------------------------------------- kernel ---

build_kernel() {
	KDIR=$BUILD/linux-$KVER
	if [ "$ISO_ONLY" = 1 ]; then
		[ -f "$KDIR/arch/x86/boot/bzImage" ] || die "--iso-only: no kernel build found"
		return 0
	fi

	fetch "$KURL" "linux-$KVER.tar.xz" "$KSHA256"
	unpack "$CACHE/linux-$KVER.tar.xz" "$KDIR" Makefile

	frag_sha=$(sha256sum "$ROOT/src/kernel.fragment" | cut -d' ' -f1)
	if [ "$(cat "$BUILD/kernel-fragment.sha256" 2>/dev/null || true)" != "$frag_sha" ]; then
		msg "configuring kernel $KVER"
		( cd "$KDIR" &&
		  make allnoconfig >/dev/null &&
		  ./scripts/kconfig/merge_config.sh -m .config "$ROOT/src/kernel.fragment" >/dev/null 2>&1 &&
		  make olddefconfig >/dev/null )
		printf '%s\n' "$frag_sha" >"$BUILD/kernel-fragment.sha256"
	fi

	# the initramfs is embedded in the kernel: one file boots on BIOS and UEFI
	cur=$(sed -n 's/^CONFIG_INITRAMFS_SOURCE="\(.*\)"/\1/p' "$KDIR/.config" 2>/dev/null || true)
	if [ "$cur" != "$BUILD/initramfs.cpio.gz" ]; then
		msg "embedding initramfs"
		( cd "$KDIR" &&
		  ./scripts/config --set-str CONFIG_INITRAMFS_SOURCE "$BUILD/initramfs.cpio.gz" &&
		  make olddefconfig >/dev/null )
	fi

	msg "building kernel (this takes a few minutes)"
	( cd "$KDIR" && make -j"$JOBS" bzImage >"$BUILD/kernel.log" 2>&1 ) || {
		tail -30 "$BUILD/kernel.log" >&2
		die "kernel build failed (see .build/kernel.log)"
	}
	grep -q "^Kernel: .*bzImage is ready" "$BUILD/kernel.log" || die "kernel build did not finish"
}

# ------------------------------------------------------------------- rootfs ---

build_rootfs() {
	msg "assembling initramfs"
	RFS=$BUILD/rootfs
	rm -rf "$RFS"
	mkdir -p "$RFS"/bin "$RFS"/sbin "$RFS"/etc "$RFS"/lib "$RFS"/dev \
	         "$RFS"/proc "$RFS"/sys "$RFS"/tmp "$RFS"/run "$RFS"/mnt "$RFS"/root \
	         "$RFS"/run/reports "$RFS"/run/state

	cp "$BUILD/busybox-$BBVER/busybox" "$RFS/bin/busybox"
	cp "$BUILD/bin/winreg" "$RFS/bin/winreg"
	cp "$BUILD/bin/wretui" "$RFS/bin/wretui"

	# applet symlinks
	( cd "$RFS/bin" && "$RFS/bin/busybox" --list | while read -r a; do
		[ "$a" = busybox ] && continue
		ln -sf busybox "$a"
	  done )

	cp "$ROOT/src/rootfs/init" "$RFS/init"
	cp -R "$ROOT/src/rootfs/lib/." "$RFS/lib/"
	cp -R "$ROOT/src/rootfs/etc/." "$RFS/etc/"
	for f in "$ROOT"/src/rootfs/bin/*; do
		cp "$f" "$RFS/bin/${f##*/}"
	done

	# version stamp
	sed -i "s/@VERSION@/$VERSION/g" "$RFS/lib/wrecovery.sh"

	chmod 0755 "$RFS/init" "$RFS/bin/busybox" "$RFS/bin/winreg" "$RFS/bin/wretui"
	for f in "$RFS"/bin/w*; do chmod 0755 "$f"; done

	"$PYTHON" "$ROOT/scripts/mkinitramfs.py" "$RFS" "$BUILD/initramfs.cpio.gz"
}

# --------------------------------------------------------------------- grub ---

BIOS_MODULES="biosdisk iso9660 part_msdos part_gpt linux normal configfile \
search search_fs_file search_label echo test ls cat sleep reboot halt"
EFI_MODULES="iso9660 part_msdos part_gpt linux normal configfile \
search search_fs_file search_label echo test ls cat sleep reboot halt"

build_grub() {
	msg "building grub boot images"
	ISOROOT=$BUILD/isoroot
	rm -rf "$ISOROOT"
	mkdir -p "$ISOROOT/boot/grub/i386-pc" "$ISOROOT/boot/efi"

	"$GRUB_MKIMAGE" -O i386-pc-eltorito -p /boot/grub -d "$GRUBDIR/i386-pc" \
		-o "$ISOROOT/boot/grub/i386-pc/eltorito.img" $BIOS_MODULES

	EFISRC=$BUILD/efisrc
	rm -rf "$EFISRC"
	mkdir -p "$EFISRC/EFI/BOOT"
	"$GRUB_MKIMAGE" -O x86_64-efi -p /boot/grub -d "$GRUBDIR/x86_64-efi" \
		-o "$EFISRC/EFI/BOOT/BOOTX64.EFI" $EFI_MODULES

	"$PYTHON" "$ROOT/scripts/mkfat.py" "$ISOROOT/boot/efi.img" "$EFISRC" --label WRE_EFI

	cp "$BUILD/linux-$KVER/arch/x86/boot/bzImage" "$ISOROOT/boot/vmlinuz"
	cat >"$ISOROOT/boot/grub/grub.cfg" <<EOF
set timeout=0
set default=0
menuentry "WRecovery $VERSION" {
    linux /boot/vmlinuz
}
EOF
}

# ---------------------------------------------------------------------- iso ---

build_iso() {
	msg "creating ISO"
	ISO=$OUT/wrecovery-$VERSION.iso
	rm -f "$ISO"
	GRUB2_MBR=""
	[ -f "$GRUBDIR/i386-pc/boot_hybrid.img" ] && GRUB2_MBR="--grub2-mbr $GRUBDIR/i386-pc/boot_hybrid.img"

	# shellcheck disable=SC2086
	"$XORRISO" -as mkisofs \
		-o "$ISO" \
		-V WRECOVERY \
		-A "WRecovery $VERSION" \
		-publisher "WRecovery" \
		-b boot/grub/i386-pc/eltorito.img \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		$GRUB2_MBR \
		-eltorito-alt-boot -e boot/efi.img -no-emul-boot \
		-isohybrid-gpt-basdat \
		"$BUILD/isoroot" >"$BUILD/iso.log" 2>&1 || {
		tail -20 "$BUILD/iso.log" >&2
		die "xorriso failed"
	}

	size_kb=$(du -k "$ISO" | cut -f1)
	size_mb=$(awk -v k="$size_kb" 'BEGIN{printf "%.2f", k/1024}')
	msg "ISO: $ISO ($size_mb MiB)"
	if [ "$SIZE_CHECK" = 1 ] && [ "$size_kb" -gt $((ISO_MAX_MB * 1024)) ]; then
		die "ISO is larger than ${ISO_MAX_MB} MiB"
	fi
	[ "$size_kb" -le $((ISO_MAX_MB * 1024)) ] || \
		warn "ISO is above the ${ISO_MAX_MB} MiB target"

	( cd "$OUT" && sha256sum "$(basename "$ISO")" >"$(basename "$ISO").sha256" )
	msg "checksum: $(cut -d' ' -f1 "$ISO.sha256")"
}

# ---------------------------------------------------------------------- main ---

build_winreg
if [ "$ISO_ONLY" = 0 ]; then
	build_busybox
fi
build_rootfs
build_kernel
build_grub
build_iso

msg "done"