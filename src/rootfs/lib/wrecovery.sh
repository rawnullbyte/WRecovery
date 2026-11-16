#!/bin/sh
# lib/wrecovery.sh - shared helpers for the WRecovery live environment
#
# Everything here is POSIX sh + busybox. No external tools.

WRE_VERSION="@VERSION@"
REPORT_DIR=/run/reports
MOUNT_ROOT=/mnt
STATE_DIR=/run/state
MOUNT_DB=$STATE_DIR/mounts.db
WIN32_CLASS_GUID='{4d36e972-e325-11ce-bfc1-08002be10318}'

msg()  { printf '%s\n' "$*"; }
warn() { printf 'warn: %s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

human_size() { # sectors (512B) -> human
	awk -v s="${1:-0}" 'BEGIN {
		b = s * 512
		split("B KiB MiB GiB TiB", u, " ")
		i = 1
		while (b >= 1024 && i < 5) { b /= 1024; i++ }
		if (i == 1) printf "%d %s", b, u[i]
		else printf "%.1f %s", b, u[i]
	}'
}

# ---------------------------------------------------------------- devices ---

# Print partition names (sdX1, nvme0n1p2, mmcblk0p1, vda1, ...) one per line.
list_partitions() {
	for d in /sys/class/block/*; do
		[ -d "$d" ] || continue
		n=${d##*/}
		case $n in
		loop*|ram*|zram*|sr*|dm-*|md*|nbd*) continue ;;
		esac
		[ -e "$d/partition" ] || continue
		printf '%s\n' "$n"
	done | sort
}

# Print whole-disk names (devices without a partition table entry).
list_disks() {
	for d in /sys/class/block/*; do
		[ -d "$d" ] || continue
		n=${d##*/}
		case $n in
		loop*|ram*|zram*|sr*|dm-*|md*|nbd*) continue ;;
		esac
		[ -e "$d/partition" ] && continue
		printf '%s\n' "$n"
	done | sort
}

part_size() { # name -> human size
	s=$(cat "/sys/class/block/$1/size" 2>/dev/null)
	human_size "${s:-0}"
}

disk_model() { # disk name -> model string
	m=$(cat "/sys/class/block/$1/device/model" 2>/dev/null)
	printf '%s' "${m:-unknown}"
}

# Is the volume BitLocker/LUKS style encrypted? Prints a short reason or "".
volume_hint() { # dev
	sig=$(dd if="$1" bs=1 skip=3 count=7 2>/dev/null)
	case $sig in
	'-FVE-FS-') printf 'BitLocker (FVE) - cannot be mounted, no unlock support yet' ;;
	*) printf '' ;;
	esac
}

# ----------------------------------------------------------------- mounts ---

db_reset() { : >"$MOUNT_DB"; }

# db_put name dev status fstype label rw
db_put() {
	grep -v "^$1|" "$MOUNT_DB" 2>/dev/null >"$MOUNT_DB.tmp"
	printf '%s|%s|%s|%s|%s|%s\n' "$1" "$2" "$3" "$4" "$5" "$6" >>"$MOUNT_DB.tmp"
	mv "$MOUNT_DB.tmp" "$MOUNT_DB"
}

db_get() { # name field(1-based)
	awk -F'|' -v n="$1" -v f="$2" '$1 == n { print $f; exit }' "$MOUNT_DB" 2>/dev/null
}

db_lines() { cat "$MOUNT_DB" 2>/dev/null; }

label_of() { # dev -> label (best effort, busybox blkid has no -o value)
	blkid "$1" 2>/dev/null | sed -n 's/.*LABEL="\([^"]*\)".*/\1/p'
}

is_mounted() { # dev
	grep -q "^$1 " /proc/mounts
}

# mount_part <name> <dev> [rw]
mount_part() {
	name=$1
	dev=$2
	rw=${3:-ro}
	mp=$MOUNT_ROOT/$name

	[ -d "$mp" ] || mkdir -p "$mp"

	if is_mounted "$dev"; then
		cur_mp=$(awk -v d="$dev" '$1 == d { print $2; exit }' /proc/mounts)
		fst=$(awk -v d="$dev" '$1 == d { print $3; exit }' /proc/mounts)
		opts=$(awk -v d="$dev" '$1 == d { print $4; exit }' /proc/mounts)
		mode=rw
		case ,$opts, in
		*,ro,*) mode=ro ;;
		esac
		if [ "$rw" = rw ] && [ "$mode" = ro ]; then
			if mount -o remount,rw "$cur_mp" 2>/dev/null; then
				mode=rw
			else
				warn "$name: could not remount read-write"
			fi
		fi
		db_put "$name" "$dev" mounted "$fst" "$(label_of "$dev")" "$mode"
		return 0
	fi

	for fs in ntfs3 ntfs vfat exfat; do
		case $fs in
		ntfs3) o="$rw" ;;
		ntfs)  o="ro,nls=utf8" ;;
		vfat)  o="$rw,iocharset=utf8,umask=022" ;;
		exfat) o="$rw,iocharset=utf8" ;;
		esac
		mount -t "$fs" -o "$o" "$dev" "$mp" 2>/dev/null
		if is_mounted "$dev"; then
			db_put "$name" "$dev" mounted "$fs" "$(label_of "$dev")" "$rw"
			return 0
		fi
	done

	hint=$(volume_hint "$dev")
	if [ -n "$hint" ]; then
		db_put "$name" "$dev" "locked: $hint" "-" "$(label_of "$dev")" "-"
	else
		db_put "$name" "$dev" "unmountable" "-" "$(label_of "$dev")" "-"
	fi
	return 1
}

unmount_all() {
	db_lines | while IFS='|' read -r name dev status fst label rw; do
		[ "$status" = mounted ] || continue
		umount "$MOUNT_ROOT/$name" 2>/dev/null && msg "unmounted $MOUNT_ROOT/$name"
	done
	db_reset
}

# disk_has_partitions <disk name>
disk_has_partitions() {
	for p in $(list_partitions); do
		case $p in
		"$1"p*|"$1"[0-9]*) return 0 ;;
		esac
	done
	return 1
}

# mount all partitions, plus whole disks that carry a filesystem directly
# (unpartitioned USB sticks are common)
mount_all() {
	rw=${1:-ro}
	[ -f "$MOUNT_DB" ] || db_reset
	rc=0
	for n in $(list_partitions); do
		dev=/dev/$n
		[ -e "$dev" ] || continue
		mount_part "$n" "$dev" "$rw" || rc=1
	done
	for d in $(list_disks); do
		disk_has_partitions "$d" && continue
		dev=/dev/$d
		[ -e "$dev" ] || continue
		case $(blkid "$dev" 2>/dev/null) in
		*TYPE=*) mount_part "$d" "$dev" "$rw" || rc=1 ;;
		esac
	done
	return $rc
}

mounted_mountpoints() {
	db_lines | awk -F'|' '$3 == "mounted" { print $1 }' | while read -r n; do
		printf '%s\n' "$MOUNT_ROOT/$n"
	done
}

# ------------------------------------------------------------ windows bits ---

# find_ci <dir> <name>: case-insensitive child lookup, prints full path
find_ci() {
	[ -d "$1" ] || return 1
	for e in "$1"/*; do
		[ -e "$e" ] || continue
		bn=${e##*/}
		if [ "$(printf '%s' "$bn" | tr 'A-Z' 'a-z')" = \
		     "$(printf '%s' "$2" | tr 'A-Z' 'a-z')" ]; then
			printf '%s\n' "$e"
			return 0
		fi
	done
	return 1
}

# win_roots <mountpoint>: prints "<root>|<label>" for every Windows tree found
win_roots() {
	mp=$1
	for cand in Windows windows WINDOWS Windows.old windows.old WINDOWS.OLD; do
		[ -d "$mp/$cand" ] || continue
		w="$mp/$cand"
		[ -f "$w/System32/config/SYSTEM" ] || \
			[ -f "$w/system32/config/SYSTEM" ] || \
			[ -f "$w/System32/config/system" ] || continue
		printf '%s\n' "$w"
	done
}

# system_hive <winroot>
system_hive() {
	for p in "System32/config/SYSTEM" "system32/config/SYSTEM" \
		 "System32/config/system" "system32/config/system"; do
		[ -f "$1/$p" ] && { printf '%s\n' "$1/$p"; return 0; }
	done
	return 1
}

software_hive() {
	for p in "System32/config/SOFTWARE" "system32/config/SOFTWARE" \
		 "System32/config/software" "system32/config/software"; do
		[ -f "$1/$p" ] && { printf '%s\n' "$1/$p"; return 0; }
	done
	return 1
}

# reg_get <hive> <path> <value>
reg_get() { winreg get "$1" "$2" "$3" 2>/dev/null; }

reg_has_key() { winreg exists "$1" "$2" >/dev/null 2>&1; }

# sam_hive_of <winroot>
sam_hive_of() {
	for p in "System32/config/SAM" "system32/config/SAM" \
		 "System32/config/sam" "system32/config/sam"; do
		[ -f "$1/$p" ] && { printf '%s\n' "$1/$p"; return 0; }
	done
	return 1
}

# win_installs: "<volume>|<windows root>|<sam hive>" per Windows install
win_installs() {
	wnet_targets | while read -r mp; do
		for w in $(win_roots "$mp"); do
			sam=$(sam_hive_of "$w" 2>/dev/null) || sam=""
			printf '%s|%s|%s\n' "${mp##*/}" "$w" "$sam"
		done
	done
}

# wnet_targets: mount points that contain a Windows install
wnet_targets() {
	mounted_mountpoints | while read -r mp; do
		[ -n "$(win_roots "$mp")" ] && printf '%s\n' "$mp"
	done
}
# ---------------------------------------------------------------- boot pass --

wrecovery_boot() {
	msg ""
	msg "  WRecovery $WRE_VERSION - offline Windows network recovery"
	msg "  -------------------------------------------------------"
	msg ""

	mount_all ro

	msg ""
	wdisks
	msg ""

	if [ "$(list_disks | wc -l)" = 0 ]; then
		msg "No disks were detected at all."
		msg ""
		wstorage
		msg ""
		msg "A controller listed with <none> has no driver in this kernel."
		msg "Otherwise check the RAID virtual disk in the controller firmware"
		msg "(Ctrl-R / F8 at boot) and the backplane cabling."
		msg ""
		msg "Recent storage kernel messages:"
		dmesg 2>/dev/null | grep -iE 'scsi|ata|nvme|ahci|raid|virtio|mmc|sd ' \
			| tail -n 8 | sed 's/^/  /'
		msg ""
	fi

	n=$(wnet_targets | wc -l)
	if [ "${n:-0}" = 0 ]; then
		msg "No Windows installation found on the attached disks."
		msg "Try 'wdisks', or 'wmount' again after attaching a disk."
	else
		wnet -q
		msg "== Recovered network configuration =="
		for f in "$REPORT_DIR"/network-*.txt; do
			[ -f "$f" ] || continue
			cat "$f"
		done
	fi

	msg ""
	msg "  Type 'whelp' for commands.  Ctrl-Alt-F2/F3 give extra consoles."
	msg ""
}
