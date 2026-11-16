# WRecovery

A ~5 MiB bootable ISO that mounts the Windows drives of a broken machine and
recovers their network configuration (static IPs, DHCP leases, DNS servers,
Wi-Fi profiles and keys, VPN phonebooks, proxy settings, hosts files) and can
clear the password of a local Windows account.

Boots on **UEFI and BIOS**, read-only by default, nothing to install and no
network access required.

```
WRecovery 0.1.0 - offline Windows network recovery
--------------------------------------------------------

DEV                 SIZE  FS      LABEL        MOUNT              WINDOWS
-------------- ---------  ------- ------------ ------------------ -------
nvme0n1       476.9 GiB  Samsung SSD 980 PRO
  nvme0n1p1   100.0 MiB  vfat    SYSTEM       /mnt/nvme0n1p1     -
  nvme0n1p2    16.0 MiB  -                    -                  -
  nvme0n1p3   476.8 GiB  ntfs3   Windows      /mnt/nvme0n1p3     Windows

== Recovered network configuration ==
 volume    : /mnt/nvme0n1p3
 computer name        DESKTOP-8H2K1
 control set          ControlSet001

== IPv4 interfaces ==
-- {621FDE4B-E28A-42F2-BE94-66EBE60E3580}
  connection           Ethernet
  adapter              Intel(R) Ethernet Connection I219-V
  ip assignment        static
  static address       10.0.20.42 / 255.255.255.0
  static gateway       10.0.20.1
  static dns           10.0.20.10,10.0.20.11

== Wi-Fi profiles (Wlansvc) ==
-- profile: CorpNet
  ssid                 CorpNet
  authentication       WPA2PSK
  encryption           AES
  key                  correct-horse-battery
```

## Quick start

Build the ISO (Linux or WSL):

```sh
./build.sh
```

Output: `out/wrecovery-0.2.0.iso` (~4.8 MiB) plus a `.sha256`.

Write it to a USB stick or feed it to a VM:

```sh
sudo dd if=out/wrecovery-0.2.0.iso of=/dev/sdX bs=4M status=progress conv=fsync
```

The image is isohybrid, so it boots from a stick as well as from a real
CD/DVD or a BMC/IPMI virtual media slot.

### Build dependencies

| distro | packages |
| --- | --- |
| Arch | `base-devel bc flex bison libelf openssl xorriso grub python3` |
| Debian/Ubuntu | `build-essential bc bison flex libelf-dev libssl-dev xorriso grub-pc-bin grub-efi-amd64-bin python3` |

`scripts/fetch-tools.sh` unpacks `xorriso` and `grub` into `.cache/tools` for
hosts where you cannot install packages (no root); `build.sh` picks them up
automatically. Everything else is downloaded into `.cache/` and verified.

Building takes a few minutes the first time (the kernel is compiled from
source), seconds afterwards.

## Using it

Boot the machine with the ISO. The environment comes up on the screen
(`tty1`), with extra consoles on `tty2`/`tty3` and on the first serial port.

Everything is mounted **read-only**. Nothing is written to the Windows volumes
unless you explicitly ask for it.

| command | what it does |
| --- | --- |
| `wdisks` | list disks, partitions, filesystems and Windows installs |
| `wmount` | mount every Windows volume read-only |
| `wmount -w sdb1` | mount one volume read-write (repair mode) |
| `wmount -u` | unmount everything |
| `wnet` | recover network config from every mounted Windows install |
| `wnet /mnt/nvme0n1p3` | recover from one volume only |
| `wusb` | mount the first removable disk read-write |
| `wsave /mnt/sdb1` | copy the reports to a USB stick |
| `wpasswd` | list local Windows accounts (RID, state, admin) |
| `wpasswd <user>` | clear that account's password and enable/unlock it |
| `wfind '*.pbk'` | search all mounted volumes |
| `winreg` | offline registry reader (`winreg ls/values/get/dump/exists`) |
| `whelp` | summary of all of the above |

The boot process already mounts the disks and writes the reports, so in the
normal case you only need to read the screen and copy the results off.

### What gets recovered

| artifact | source |
| --- | --- |
| interface list with friendly names and adapter descriptions | `SYSTEM\ControlSet00X\Control\Network` |
| static IP / mask / gateway / DNS, DHCP flag | `SYSTEM\...\Services\Tcpip\Parameters\Interfaces\{GUID}` |
| DHCP lease, DHCP server, DHCP DNS, last SSID | same key (`Dhcp*` values) |
| IPv6 settings | `SYSTEM\...\Services\Tcpip6\Parameters\Interfaces\{GUID}` |
| hostname, domain, DNS suffix, search list | `SYSTEM\...\Services\Tcpip\Parameters` |
| persistent static routes | `SYSTEM\...\Tcpip\Parameters\PersistentRoutes` |
| Wi-Fi profiles, SSIDs, PSK passphrases | `ProgramData\Microsoft\Wlansvc\Profiles\Interfaces\*\*.xml` |
| VPN / dial-up entries (host, device, user) | `ProgramData\Microsoft\Network\Connections\Pbk\rasphone.pbk` |
| proxy / PAC settings | `SOFTWARE\Microsoft\Windows\CurrentVersion\Internet Settings` |
| hosts entries | `Windows\System32\drivers\etc\hosts` |
| machine identity, Windows version | `SYSTEM` / `SOFTWARE` hives |

Output lands in `/run/reports`:

```
network-<vol>.txt      readable report
network-<vol>.tsv      machine readable: context<TAB>field<TAB>value
restore-<vol>.cmd      netsh script that rebuilds the static configuration
wifi-<vol>/*.xml       WLAN profiles, importable with `netsh wlan add profile`
```

`Windows.old` next to the live install is scanned as well, so a downgrade or
reinstall does not lose the old network settings.

## Resetting a Windows password

Lost the local administrator password? The SAM hive is on the same disk:

```
wpasswd                       # list local accounts
wpasswd Administrator         # clear it, enable + unlock the account
```

What this does (the same thing ntpasswd/chntpw has done for 20 years):

* sets the LM and NT hash *lengths* in the account's `V` record to zero, which
  makes Windows treat the password as empty,
* clears the disabled flag and the lockout counter in the account's `F`
  record, and sets "password never expires",
* recomputes the hive's base-block checksum, which Windows validates on load,
* writes a copy of the SAM hive next to it (`SAM.bak-<timestamp>`) and into
  `/run/reports` (so `wsave` carries it off the machine) before touching it.

Then boot Windows and log in with an empty password, and set a new one
immediately.

Limits of this approach:

* **Microsoft accounts** (the login is an e-mail address) are validated
  online — clearing the cached password does not let you into the profile.
  Local accounts only.
* The volume is remounted **read-write** for the operation. That is the one
  place where WRecovery writes to a Windows disk, and it asks for confirmation
  first.
* If `SAM.LOG1`/`SAM.LOG2` are not empty the hive has unflushed changes and
  Windows replays that log on boot, which can undo the reset. Boot Windows once
  and shut it down cleanly, then run `wpasswd` again.
* No BitLocker/Syskey handling: if the SYSTEM hive is protected by a Syskey
  startup password this is out of scope.

## How it works

```
build.sh
 ├─ fetch + verify    kernel tarball, busybox tarball (sha256 pinned)
 ├─ build             winreg (static C), busybox (static), linux bzImage
 ├─ initramfs         busybox + scripts + winreg -> cpio.gz
 ├─ kernel            CONFIG_INITRAMFS_SOURCE embeds the initramfs
 └─ iso               grub BIOS eltorito.img + grub EFI image in a FAT16
                      El Torito partition, isohybrid MBR/GPT
```

* **One kernel file, both firmwares.** GRUB's `i386-pc-eltorito` image boots
  the kernel on BIOS; a GRUB `x86_64-efi` binary in a small FAT image boots it
  on UEFI. The kernel has the initramfs built in, so there is no initrd file
  to keep in sync.
* **Small on purpose.** The kernel is configured from `allnoconfig` plus an
  explicit list (`src/kernel.fragment`): no modules, no DRM, no networking
  stack, only the storage controllers and the three filesystems that matter
  (NTFS3, exFAT, FAT). That is the difference between 7.7 MiB and 2.1 MiB of
  kernel.
* **Offline registry parsing.** `src/winreg.c` is a ~1000 line read-only parser
  for the Windows `regf` hive format (cells, hbins, `nk`/`vk`/`lf`/`li`/`ri`,
  big-data values, UTF-16 strings). No Windows, no `chntpw`, no Python in the
  image. It is differential-tested against
  [python-registry](https://github.com/williballenthin/python-registry) over
  ~50k values on every CI run.
* **Read-only by default.** All mounts are `ro`; `wmount -w` only ever writes
  when you name the volume.

## Tests

```sh
./tests/run-tests.sh
```

* builds an NTFS test volume (real Windows hive, WLAN profile XMLs, VPN
  phonebook, hosts file) inside a partitioned disk image,
* boots the ISO in QEMU on **BIOS** and on **UEFI** with that disk attached,
* checks that the disk is mounted, the interfaces are parsed, the Wi-Fi key is
  recovered, the VPN host and hosts entries show up,
* drives the guest shell over the serial console, clears a Windows password
  with `wpasswd` (and checks the SAM hive afterwards), mounts a second disk and
  verifies that `wsave` writes the reports to it,
* compares `winreg` against `python-registry` for every key and value of the
  sample hive.

`tests/run-tests.sh --iso path/to.iso` reuses an existing ISO, `--no-uefi`
skips the UEFI pass.

## CI

`.github/workflows/build.yml`

* on every push/PR: installs the build dependencies, caches the kernel tree,
  builds with `--size-check` (fails above 10 MiB), runs the full test suite,
  uploads `wrecovery-<version>.iso` as an artifact.
* on a `v*` tag: the same, then attaches the ISO and its checksum to a GitHub
  release.

## Limitations

* **BitLocker** volumes are detected and reported, but not unlocked. There is
  no `cryptsetup` in the image (it would not fit the size budget).
* **Password reset** only works for local accounts, and only by *clearing* the
  password (the ntpasswd technique). Setting a specific new hash offline would
  need the SAM boot key and RC4/AES re-encryption, which is deliberately not
  implemented — a wrong hash leaves the account unusable.
* **DPAPI-protected Wi-Fi keys** (`<protected>true</protected>`, typical for
  enterprise profiles and for profiles created after Windows 10 1803) are
  reported, and the profile XML is exported so Windows can import it. Decrypting
  them offline needs the DPAPI master keys and the user's password — out of
  scope here.
* **Dynamic disks / Storage Spaces** are not assembled. Basic and GPT disks,
  including all partitions, are.
* **No network stack** in the image, by design: recovery data goes to a USB
  stick (`wsave`) instead of over the network.
* 32-bit UEFI is not supported (`BOOTX64.EFI` only).

## Layout

```
build.sh                    one-command build
src/winreg.c                offline registry hive parser
src/kernel.fragment         minimal kernel config (allnoconfig base)
src/busybox.config          busybox config (static, no network applets)
src/rootfs/init             PID 1
src/rootfs/lib/wrecovery.sh mount/scan helpers
src/rootfs/bin/w*           wdisks, wmount, wnet, wsave, wfind, wusb, whelp
scripts/mkfat.py            FAT12/16 image builder (El Torito EFI boot image)
scripts/mkinitramfs.py      cpio.gz builder with static /dev nodes
scripts/mkdiskimg.py        MBR/GPT test disk builder
scripts/fetch-tools.sh      rootless xorriso/grub fetch
tests/run-tests.sh          end-to-end BIOS+UEFI tests
```