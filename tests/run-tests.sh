#!/bin/sh
# run-tests.sh - end-to-end tests for WRecovery
#
#   tests/run-tests.sh              build fixtures, boot BIOS + UEFI, assert
#   tests/run-tests.sh --no-uefi    skip the UEFI pass
#   tests/run-tests.sh --iso FILE   use an existing ISO
#   tests/run-tests.sh --keep       keep .tmp/tests around for debugging
#
# Needs qemu-system-x86_64 (or WRE_QEMU=/path/to/qemu) plus, for a realistic
# fixture, mkntfs + ntfs-3g. Without those it falls back to a FAT test volume,
# which still exercises everything except the NTFS driver itself.

set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP=$ROOT/.tmp/tests
CACHE=$ROOT/.cache
QEMU=${WRE_QEMU:-}
UEFI=1
ISO=""
KEEP=0
TIMEOUT=${WRE_BOOT_TIMEOUT:-120}

while [ $# -gt 0 ]; do
	case $1 in
	--iso) ISO=$2; shift 2 ;;
	--no-uefi) UEFI=0; shift ;;
	--keep) KEEP=1; shift ;;
	-h|--help) sed -n '2,13p' "$0"; exit 0 ;;
	*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
done

pass=0
fail=0
ok()   { pass=$((pass+1)); printf '  \033[32mok\033[0m   %s\n' "$*"; }
bad()  { fail=$((fail+1)); printf '  \033[31mFAIL\033[0m %s\n' "$*"; }
head_() { printf '\n\033[1m%s\033[0m\n' "$*"; }

# --------------------------------------------------------------- prerequisites

head_ "prerequisites"

if [ -z "$QEMU" ]; then
	for c in qemu-system-x86_64 "$ROOT/.cache/qemu/usr/bin/qemu-system-x86_64"; do
		if [ -x "$c" ] || command -v "$c" >/dev/null 2>&1; then
			QEMU=$c
			break
		fi
	done
fi
[ -n "$QEMU" ] || { echo "qemu-system-x86_64 not found (set WRE_QEMU)" >&2; exit 1; }
QEMU_LIB=""
case $QEMU in
"$ROOT/.cache/qemu/"*) QEMU_LIB=$ROOT/.cache/qemu/usr/lib ;;
esac
ok "qemu: $QEMU"

if [ -z "$ISO" ]; then
	ISO=$(ls -t "$ROOT"/out/wrecovery-*.iso 2>/dev/null | head -1 || true)
fi
[ -n "$ISO" ] && [ -f "$ISO" ] || { echo "no ISO found - run ./build.sh first" >&2; exit 1; }
ok "iso: $ISO ($(du -k "$ISO" | cut -f1) KiB)"

# OVMF for the UEFI pass
OVMF=""
if [ "$UEFI" = 1 ]; then
	for d in "$ROOT/.cache/qemu/usr/share/edk2/x64" /usr/share/edk2/x64 \
	         /usr/share/edk2-ovmf /usr/share/OVMF /usr/share/qemu; do
		[ -f "$d/OVMF_CODE.4m.fd" ] && { OVMF=$d; break; }
		[ -f "$d/OVMF_CODE.fd" ] && { OVMF=$d; break; }
	done
	if [ -n "$OVMF" ]; then
		ok "ovmf: $OVMF"
	else
		echo "  --   ovmf not found, skipping the UEFI pass"
		UEFI=0
	fi
fi

# ------------------------------------------------------------------- fixtures

head_ "fixtures"
mkdir -p "$TMP"
rm -rf "$TMP"/*

# `timeout` cannot run a shell function, so wrap the emulator in a script
mkdir -p "$TMP"
cat >"$TMP/qemu" <<EOF
#!/bin/sh
LD_LIBRARY_PATH="\${LD_LIBRARY_PATH:-}$QEMU_LIB" exec "$QEMU" "\$@"
EOF
chmod +x "$TMP/qemu"


HIVE_URL=https://github.com/williballenthin/python-registry/raw/master/testing/reg_samples/new_log_1/SYSTEM
HIVE_SHA=6b5ef3ad32ddd095b2d40671f5d1815d6563a40d3eb9d9ba3d8e93b0c444ad6b
HIVE=$CACHE/SYSTEM-test
if [ ! -f "$HIVE" ] || [ "$(sha256sum "$HIVE" | cut -d' ' -f1)" != "$HIVE_SHA" ]; then
	echo "  ..  downloading test hive (7.3 MiB)"
	curl -fsSL --retry 3 -o "$HIVE.part" "$HIVE_URL"
	mv "$HIVE.part" "$HIVE"
fi
[ "$(sha256sum "$HIVE" | cut -d' ' -f1)" = "$HIVE_SHA" ] || { echo "bad hive checksum" >&2; exit 1; }
ok "sample SYSTEM hive"

TREE=$TMP/tree
mkdir -p "$TREE/Windows/System32/config" "$TREE/Windows/System32/drivers/etc"
mkdir -p "$TREE/ProgramData/Microsoft/Wlansvc/Profiles/Interfaces/{621FDE4B-E28A-42F2-BE94-66EBE60E3580}"
mkdir -p "$TREE/ProgramData/Microsoft/Network/Connections/Pbk"
cp "$HIVE" "$TREE/Windows/System32/config/SYSTEM"
python3 "$ROOT/tests/mksam.py" "$TREE/Windows/System32/config/SAM" >/dev/null

cat >"$TREE/Windows/System32/drivers/etc/hosts" <<'EOF'
127.0.0.1       localhost
10.0.0.5        intranet.corp.example.com intranet
EOF

cat >"$TREE/ProgramData/Microsoft/Wlansvc/Profiles/Interfaces/{621FDE4B-E28A-42F2-BE94-66EBE60E3580}/CorpNet.xml" <<'EOF'
<?xml version="1.0"?>
<WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">
	<name>CorpNet</name>
	<SSIDConfig><SSID><name>CorpNet</name></SSID></SSIDConfig>
	<connectionType>ESS</connectionType>
	<MSM><security><authEncryption>
		<authentication>WPA2PSK</authentication><encryption>AES</encryption>
	</authEncryption><sharedKey>
		<keyType>passPhrase</keyType><protected>false</protected>
		<keyMaterial>correct-horse-battery</keyMaterial>
	</sharedKey></security></MSM>
</WLANProfile>
EOF

cat >"$TREE/ProgramData/Microsoft/Wlansvc/Profiles/Interfaces/{621FDE4B-E28A-42F2-BE94-66EBE60E3580}/GuestWifi.xml" <<'EOF'
<?xml version="1.0"?>
<WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">
	<name>GuestWifi</name>
	<SSIDConfig><SSID><name>GuestWifi</name></SSID></SSIDConfig>
	<MSM><security><authEncryption>
		<authentication>WPA2PSK</authentication><encryption>AES</encryption>
	</authEncryption><sharedKey>
		<keyType>passPhrase</keyType><protected>true</protected>
		<keyMaterial>01000000D08C9DDF0115D1118C7A00C04FC297EB</keyMaterial>
	</sharedKey></security></MSM>
</WLANProfile>
EOF

cat >"$TREE/ProgramData/Microsoft/Network/Connections/Pbk/rasphone.pbk" <<'EOF'
[Corp VPN]
Encoding=1
PBVersion=4
Type=2
VpnStrategy=1
HostName=vpn.corp.example.com
Domain=corp.example.com
UserName=alice
Device=WAN Miniport (IKEv2)
EOF
ok "windows tree"

# --- the Windows volume: NTFS when the tools are there, FAT otherwise -------
MKNTFS=$(command -v mkntfs || echo "$ROOT/.cache/tools/usr/bin/mkntfs")
NTFS3G=$(command -v ntfs-3g || echo "$ROOT/.cache/tools/usr/bin/ntfs-3g")
NTFSLIB=$ROOT/.cache/tools/usr/lib
[ -x "$MKNTFS" ] || MKNTFS=""

WINFS=$TMP/win.fs

fat_volume() {
	"$ROOT/scripts/mkfat.py" "$WINFS" "$TREE" --label WIN_TEST --size 50331648 >/dev/null
	ok "fat test volume (ntfs-3g unavailable: ntfs driver not exercised)"
}

if [ -n "$MKNTFS" ] && [ -x "$NTFS3G" ]; then
	truncate -s 48M "$WINFS"
	LD_LIBRARY_PATH=$NTFSLIB "$MKNTFS" -F -q -L WIN_TEST -c 4096 "$WINFS" \
		>/dev/null 2>&1 || true
	mkdir -p "$TMP/mnt"
	LD_LIBRARY_PATH=$NTFSLIB "$NTFS3G" -o no_def_opts,silent "$WINFS" \
		"$TMP/mnt" >/dev/null 2>&1 || true
	if mountpoint -q "$TMP/mnt" 2>/dev/null; then
		cp -a "$TREE"/. "$TMP/mnt"/
		sync
		fusermount3 -u "$TMP/mnt" 2>/dev/null || \
			fusermount -u "$TMP/mnt" 2>/dev/null || \
			umount "$TMP/mnt" 2>/dev/null || true
		ok "ntfs test volume (ntfs3 driver will be exercised)"
	else
		fat_volume
	fi
else
	fat_volume
fi

python3 "$ROOT/scripts/mkdiskimg.py" --fs "$WINFS" --out "$TMP/windisk.img" >/dev/null
ok "partitioned disk image"

# --- a second install with a static IP (synthetic hive) ---------------------
mkdir -p "$TMP/static-tree/Windows/System32/config"
python3 "$ROOT/tests/mkstatic.py" "$TMP/static-tree/Windows/System32/config/SYSTEM" >/dev/null
python3 "$ROOT/scripts/mkfat.py" "$TMP/static.img" "$TMP/static-tree" \
	--label LABDISK --size 4194304 >/dev/null
ok "static-ip test volume"

# --- a "USB stick" for wsave ------------------------------------------------
mkdir -p "$TMP/usbtree"
echo "usb stick" >"$TMP/usbtree/README.txt"
python3 "$ROOT/scripts/mkfat.py" "$TMP/usb.img" "$TMP/usbtree" --label WREUSB --size 8388608 >/dev/null
ok "usb image"

# ------------------------------------------------------------------ boot runs

run_vm() { # logfile extra-qemu-args... (reads commands from $CMDS)
	log=$1
	shift
	(
		sleep 25
		if [ -n "${CMDS:-}" ]; then
			printf '%s\n' "$CMDS"
			sleep 6
		fi
	) | timeout "$TIMEOUT" "$TMP/qemu" -m 1024 -cdrom "$ISO" -net none -nographic \
		-no-reboot -display none \
		-drive file="$TMP/windisk.img",format=raw,if=ide \
		-drive file="$TMP/usb.img",format=raw,if=ide \
		-drive file="$TMP/static.img",format=raw,if=ide \
		"$@" >"$log" 2>&1 || true
}

expect() { # log pattern description
	if grep -aqF -- "$2" "$1"; then
		ok "$3"
	else
		bad "$3 (missing: $2)"
	fi
}

head_ "BIOS boot"
CMDS='echo MARKER-BIOS
wpasswd -n sda1 alice
wpasswd -y sda1 alice
winreg sam-users /mnt/sda1/Windows/System32/config/SAM
wmount -w sdb
wsave /mnt/sdb
cat /mnt/sdb/wrecovery-reports-*/restore-sdc.cmd
echo MARKER-DONE'
run_vm "$TMP/bios.log" -boot d
expect "$TMP/bios.log" "WRecovery" "kernel + initramfs started"
expect "$TMP/bios.log" "WIN_TEST" "Windows volume detected"
expect "$TMP/bios.log" "Windows" "Windows install found"
expect "$TMP/bios.log" "== IPv4 interfaces ==" "interfaces parsed"
expect "$TMP/bios.log" "CorpNet" "wifi profile found"
expect "$TMP/bios.log" "correct-horse-battery" "wifi key recovered"
expect "$TMP/bios.log" "DPAPI protected" "protected key flagged"
expect "$TMP/bios.log" "vpn.corp.example.com" "VPN host found"
expect "$TMP/bios.log" "intranet.corp.example.com" "hosts file read"
expect "$TMP/bios.log" "MARKER-BIOS" "shell reachable on ttyS0"
expect "$TMP/bios.log" "static address       192.168.77.10 / 255.255.255.0" "static ip recovered"
expect "$TMP/bios.log" "connection           Ethernet 2" "friendly interface name"
expect "$TMP/bios.log" 'netsh interface ipv4 set address name="Ethernet 2" static 192.168.77.10 255.255.255.0 192.168.77.1' "restore script: static address"
expect "$TMP/bios.log" 'netsh interface ipv4 set dns name="Ethernet 2" static 192.168.77.53 primary' "restore script: dns"
expect "$TMP/bios.log" "account   : disabled -> enabled/unlocked" "wpasswd dry run shows the change"
expect "$TMP/bios.log" "done: password cleared, account enabled" "wpasswd cleared the password"
expect "$TMP/bios.log" "1001       alice                    Alice Example" "sam-users lists the account"
expect "$TMP/bios.log" "reports copied to /mnt/sdb/wrecovery-reports-" "wsave wrote to the usb stick"
expect "$TMP/bios.log" "WRecovery network configuration report" "saved report is readable"

head_ "UEFI boot"
if [ "$UEFI" = 1 ]; then
	if [ -f "$OVMF/OVMF_CODE.4m.fd" ]; then
		CODE=$OVMF/OVMF_CODE.4m.fd
		VARS=$OVMF/OVMF_VARS.4m.fd
	else
		CODE=$OVMF/OVMF_CODE.fd
		VARS=$OVMF/OVMF_VARS.fd
	fi
	cp "$VARS" "$TMP/vars.fd"
	CMDS=""
	run_vm "$TMP/uefi.log" \
		-drive if=pflash,format=raw,readonly=on,file="$CODE" \
		-drive if=pflash,format=raw,file="$TMP/vars.fd"
	expect "$TMP/uefi.log" "WRecovery" "kernel + initramfs started"
	expect "$TMP/uefi.log" "WIN_TEST" "Windows volume detected"
	expect "$TMP/uefi.log" "correct-horse-battery" "wifi key recovered"
else
	echo "  --   skipped"
fi

head_ "SAM password reset (host side)"
SAMDIR=$TMP/samtest
mkdir -p "$SAMDIR"
python3 "$ROOT/tests/mksam.py" "$SAMDIR/SAM" >/dev/null
cp "$SAMDIR/SAM" "$SAMDIR/SAM.orig"

WINREG=$ROOT/.build/bin/winreg
if [ ! -x "$WINREG" ]; then
	WINREG=$TMP/winreg
	gcc -Os -static -s -o "$WINREG" "$ROOT/src/winreg.c" 2>/dev/null || WINREG=""
fi
if [ -n "$WINREG" ]; then
	"$WINREG" sam-users "$SAMDIR/SAM" >"$SAMDIR/users.txt" 2>&1
	if grep -q "Administrator" "$SAMDIR/users.txt" &&
	   grep -q "alice" "$SAMDIR/users.txt"; then
		ok "sam-users lists local accounts"
	else
		bad "sam-users did not list the fixture accounts"
	fi
	if grep -q "alice.*disabled" "$SAMDIR/users.txt"; then
		ok "sam-users shows the disabled flag"
	else
		bad "sam-users missed the disabled flag"
	fi

	"$WINREG" sam-reset "$SAMDIR/SAM" alice >"$SAMDIR/reset.txt" 2>&1
	if grep -q "done: password cleared" "$SAMDIR/reset.txt"; then
		ok "sam-reset reported success"
	else
		bad "sam-reset failed: $(tail -1 "$SAMDIR/reset.txt")"
	fi

	if python3 -c 'import Registry' 2>/dev/null; then
		if python3 "$ROOT/tests/test-sam.py" "$SAMDIR/SAM.orig" "$SAMDIR/SAM" 1001 \
				>"$SAMDIR/verify.txt" 2>&1; then
			ok "reset verified with python-registry ($(tail -1 "$SAMDIR/verify.txt" | sed 's/^ *//'))"
		else
			bad "reset verification failed"
			cat "$SAMDIR/verify.txt"
		fi
	else
		echo "  --   python-registry not installed, skipping verification"
	fi

	"$WINREG" sam-reset "$SAMDIR/SAM" Administrator --dry-run >"$SAMDIR/dry.txt" 2>&1
	if grep -q "dry run: nothing written" "$SAMDIR/dry.txt" &&
	   cmp -s "$SAMDIR/SAM" "$SAMDIR/SAM"; then
		ok "--dry-run does not write"
	else
		bad "--dry-run wrote to the hive"
	fi
else
	echo "  --   no winreg binary, skipping SAM tests"
fi

head_ "regression: winreg vs python-registry"
if python3 -c 'import Registry' 2>/dev/null; then
	if python3 "$ROOT/tests/compare-registry.py" "$HIVE" \
			>"$TMP/compare.log" 2>&1; then
		ok "hive parser matches python-registry ($(tail -1 "$TMP/compare.log"))"
	else
		bad "hive parser differs from python-registry"
		tail -5 "$TMP/compare.log"
	fi
else
	echo "  --   python-registry not installed (pip install python-registry)"
fi

head_ "result"
printf '  %d passed, %d failed\n' "$pass" "$fail"
[ "$KEEP" = 1 ] || [ "$fail" = 0 ] || echo "  logs kept in $TMP"
[ "$fail" = 0 ]