# Compound AIC installer-interface policy

This is a portable userspace device quirk shipped with the driver package. It
uses Linux's documented USB interface-authorization API. It does not add a dummy
kernel driver, race `usb-storage` for ownership, change a USB storage blacklist,
or depend on the PC's bus/port numbering.

The verified hardware revision exposes a separate Windows-installer disk beside
the AIC Wi-Fi device. The policy matches all of these facts before acting:

- Direct parent hub `214b:7260`, revision `0100`, USB hub class and four ports.
- Storage device `1111:1111`, revision `1001`, `FLASH` manufacturer/product,
  firmware serial `12345FLN1H`, one configuration and one interface.
- That interface is number zero, bulk-only transparent-SCSI storage (`08/06/50`)
  with two endpoints and no additional device functions.
- An authorized, direct sibling AIC device `a69c:8d80` or `a69c:8d81` exists.
- Exactly one removable SCSI disk of 7,944 sectors exists and has no stacked
  block holders. The device node must match its sysfs major/minor.

Only that installer's **interface** is deauthorized. Wi-Fi, Bluetooth, parent
hub, other ports and USB device authorization stay unchanged. No SCSI vendor
command, mode switch, filesystem read/write or persistent/NVM setting is used.
A complete unplug/replug restores default interface authorization. A different
revision/serial/size is intentionally left available until its identity has been
verified; this fingerprint is hardware matching, not cryptographic authentication.

## Event ordering and mounted storage

The rule handles the storage-interface add, AIC-device add and installer-disk
add events. The helper is a short synchronous task with no sleeps, polling,
worker daemon or deferred command. If the other function or disk is not present
yet, it does nothing; the later event evaluates the same complete match.

When a disk exists, the helper opens it read-only with `O_EXCL` and keeps that
claim through deauthorization. A mounted or exclusively claimed filesystem in
any mount namespace makes this fail; stacked holders also prevent action. This
does not detect every possible nonexclusive raw-disk reader. The helper never
unmounts anything. A busy or ambiguous device stays available; unmount/replug is
required before the policy can apply later.

The helper pins the exact `authorized` sysfs file descriptor before its final
identity/generation check and writes through that descriptor. A device replaced
at the same port cannot redirect the old decision to the new pathname.

This userspace policy can remove a block device after initial kernel discovery;
it does not promise that the USB storage identity never enumerates. On a normal
fresh plug, the disk-add action runs before udev publishes the completed event
to desktop subscribers. No matching filesystem is mounted by the helper.

## Installation and removal

The normal repository `install.sh` installs Python 3 and these two files. For a
package or a policy-only installation, stage/install just the policy:

```sh
DESTDIR=/package/staging sh udev/install-policy.sh
# Or, to install only these files on the live host:
sudo sh udev/install-policy.sh
sudo udevadm control --reload-rules
```

Installed paths:

- `/usr/libexec/aic8800-installer-policy` (Python 3 standard library only).
- `/usr/lib/udev/rules.d/59-aic8800-installer.rules`.

The standalone installer does not build/load modules, alter firmware, reload
rules or trigger unrelated devices. Replug the adapter to apply the policy. To
disable it locally, override that rules filename with a `/dev/null` symlink in
`/etc/udev/rules.d`, reload udev rules, and replug. Remove the two package files
when uninstalling the policy. Existing deauthorization lasts until replug or an
explicit administrator authorization/probe; removing a rule alone does not
change the current interface state.

For metadata-only diagnosis (no disk open or authorization change):

```sh
/usr/libexec/aic8800-installer-policy --check /sys/bus/usb/devices/DEVICE
```

`--apply` is the fixed root-only udev entry point. Kernel DEVPATHs and absolute
sysfs paths are accepted only below `/sys/devices`; no arbitrary output path,
command, bus-specific selector or modeswitch byte sequence can be supplied.

The old automatic `1111:1111` vendor-command rule is removed from the package:
that VID/PID is not a unique AIC identity, and the separate compound installer
is not the same topology as standalone Pandora mode switching. Manual Pandora
instructions remain in the root README. Administrator modeswitch files and
unrelated rules are preserved; external broad rules must be removed separately
before relying on this package's behavior.

## Validation and upstream placement

The helper has focused actual-Python tests for both USB arrival orders, late disk
publication, repeat application, unrelated identities/ports/revisions, mounted
claim refusal, path validation and replacement of the authorization pathname.
The installed file layout is checked through a `DESTDIR` staging test, and the
rule passes `udevadm verify`. These host doubles do not replace a live policy
check on the real installer and do not establish a match for every AIC product.

This policy belongs in device-support packaging/udev, using the existing kernel
ABI; it is not a submitted or accepted kernel quirk. A kernel `unusual_devs`
entry that ignores every `1111:1111` disk would be too broad. A companion USB
driver cannot reliably beat the generic storage driver merely by registering
another broad ID and later rejecting unrelated devices.

Primary references:

- [Linux USB authorization API](https://docs.kernel.org/usb/authorization.html).
- [udev rule/event semantics](https://www.freedesktop.org/software/systemd/man/latest/udev.html).
- [Linux open(2), O_EXCL for block devices](https://man7.org/linux/man-pages/man2/open.2.html).
