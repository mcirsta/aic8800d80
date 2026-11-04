# AIC compound installer quirk

`0001-ignore-aic-compound-installer.patch` is a draft change to the normal Linux
USB-storage driver. It declines the verified adapter's separate Windows
installer before registering a SCSI host or disk. It sends no eject, vendor
mode-switch or firmware command and requires no userspace suppression daemon.

The quirk matches the observed FLASH revision, descriptors and serial under
the matching four-port hub, with FLASH on relative internal port 2 and an AIC
ROM/runtime sibling on port 1. These are ports inside the adapter, independent
of the computer's USB bus/port. The AIC driver need not be ready, configured or
authorized: cached numeric descriptor identity is sufficient. No sibling mutex
or readiness-dependent check is used. Other devices continue normal probing.

This is a normal kernel patch, separate from the Wi-Fi module. Building or
installing `aic8800_fdrv.ko` alone does not apply it. Apply the patch to the
matching kernel source and include it in the distribution's normal kernel or
USB-storage module build; keep that kernel's module ABI, signing and initramfs
requirements. Preserve a working original module/kernel and boot fallback.
Do not replace or unload USB-storage while it serves mounted/in-use media.

The patch was generated against the official CachyOS 7.3.1-1 source. Its normal
module build retains stock storage IDs, exports and ordinary probing. It has no
test-only forced rejection, renamed driver, dynamic-ID injection or symbol hook.
The narrow initializer passed 82 focused source tests. On the verified board,
a short live test of the normal module returned `-ENODEV` before SCSI/disk
publication with the userspace policy removed. The separate empty card reader
returned through ordinary probing; Wi-Fi, Bluetooth, installed DKMS/firmware
and other storage were preserved.

The local 7.3.1-1-cachyos installation now selects the patched normal module and
contains its exact bytes in the rebuilt initramfs, with matching Limine hashes
and the LTS fallback unchanged. The userspace helper/rule and temporary guard
are removed. One subsequent physical unplug/replug also left the installer
authorized without a SCSI host or disk, matching the AIC ROM sibling on port 1.
Reboot and successful ROM-to-runtime enumeration remain untested. This local
override is kernel-version specific and must be rebuilt for a kernel upgrade
until the patch is integrated in the kernel package.

That fresh plug separately exposed a failure in the preserved installed AIC
loader: it could not open the existing patch-table firmware file and stayed in
`a69c:8d80` ROM mode without a network interface. Kernel logs show this failure
before the FLASH child is enumerated or this initializer runs. It is not a
passing Wi-Fi hot-plug result. The development loader uses `request_firmware()`;
the installed DKMS loader still uses `filp_open()` and has not been replaced.

An absent AIC sibling remains a nonmatch. A ROM-to-runtime enumeration gap may
therefore leave storage available; no unsafe assumption about a missing device,
unbounded deferred probe or bus-notifier policy is added. The observed revision
and ordinary board plug ordering need validation before claiming broader model
support. This patch is not yet submitted or accepted upstream.

Mainline has accepted installer ignore quirks, including the
[2025 Realtek change](https://patchew.org/linux/20250813162415.2630-1-zenmchen@gmail.com/).
Acceptance of this draft remains a separate review question: the generic USB
identity and a working installer require a clear hardware/model justification.
The preference to remove the Windows disk is not presented as a demonstrated
failure of ordinary USB-storage discovery.
