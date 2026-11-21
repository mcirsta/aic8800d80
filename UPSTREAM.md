# AIC8800D80 USB mainline readiness

Reviewed 21 November 2026 at `d47a467`. The installed driver works for basic
station use on the tested D80 U02 USB dongle. Its [live validation](USB-VALIDATION.md)
covers reload, physical replug, and S3 resume. The current DKMS tree still needs
the work below before it is suitable for submission to `wireless-next`.

## Coordinate with the AIC upstream work

AIC's 16 November [v5 series](https://lkml.rescloud.iu.edu/hypermail/linux/kernel/2611.2/00340.html)
targets D80 U02 and D80N **SDIO** station mode. It proposes a shared FullMAC
implementation, firmware loading and maintainership for that scope. It does
not cover USB. The [mainline wireless tree checked for this review](https://github.com/torvalds/linux/tree/c44104f3642da720164042623e4832836aefdd8e/drivers/net/wireless)
has no AIC directory. Compare USB and SDIO message layouts before sharing code,
and aim for a USB transport with the minimum chip-specific code on top of a
shared core where the ABI matches. Agree on USB review and maintenance ownership
with the AIC contributors before submitting a competing FullMAC core.

## Submission blockers

| Blocker | Evidence in the current branch | What would close it |
| --- | --- | --- |
| Exact USB firmware and redistribution | The [test image](USB-VALIDATION.md) and three other required files differ from the files with the same names in [linux-firmware WHENCE](https://kernel.googlesource.com/pub/scm/linux/kernel/git/firmware/linux-firmware/+/a1ebb5ad7eae3caca60b8b3db5792cf43339ba58/WHENCE). One required patch file is missing there. The driver's GPL text covers source. | Obtain a vendor-approved USB set with documented version, chip IDs, hashes, required filenames and redistribution terms. Stage `WHENCE` entries, declare the actual files with `MODULE_FIRMWARE()` and test clean firmware installation and ROM boot. |
| Regulatory and power enforcement | [`custregd`](drivers/aic8800/aic8800_fdrv/rwnx_mod_params.c) defaults to true while its parameter description says zero. The runtime prints the permissive rules banner. [`get_tx_power`](drivers/aic8800/aic8800_fdrv/rwnx_main.c) returns a configured maximum and does not query the effective radio setting. | Remove permissive and custom channel overrides from the submission. Apply cfg80211 channel, DFS and power restrictions to the firmware, intersect them with hardware limits, and verify the effective settings. Restrict the supported channels and modes to what the USB firmware can enforce. |
| In-tree build and interfaces | The [Kconfig](drivers/aic8800/Kconfig) and [Makefiles](drivers/aic8800/aic8800_fdrv/Makefile) belong to an external vendor build. They carry kernels and platforms outside the upstream scope, WEXT handlers and a private TCP ACK filter. The source also retains optional calibration file I/O and a firmware dump userspace helper. | Make a small native Kbuild/Kconfig series for D80 U02 USB station mode on current `wireless-next`. Trim WEXT, private controls, the ACK filter, obsolete compatibility branches and unused platform paths. Drop direct file I/O and private VFS namespace imports from the candidate. Add a `MAINTAINERS` entry with a committed owner. [Wireless submission guidance](https://wireless.docs.kernel.org/en/latest/en/developers/documentation/submittingpatches.html) |
| Per-device state and ownership | The [loader](drivers/aic8800/aic_load_fw/aicwf_usb.c) keeps `fw_loaded` and chip revision in globals. The [runtime platform](drivers/aic8800/aic8800_fdrv/rwnx_platform.c) uses a global platform pointer. The live results exercise one adapter at a time. | Give each USB device its own state, define the ROM-to-runtime handoff, and review USB/firmware/cfg80211 teardown serialization. Validate two adapters plus removal during scan/connect/TX and one bounded firmware recovery under kernel diagnostics. |
| Accurate cfg80211 capabilities and reports | [`rwnx_main.c`](drivers/aic8800/aic8800_fdrv/rwnx_main.c) advertises broad cipher and interface sets. Station reporting now propagates firmware query failures, checks the rate data and omits interface totals. A short D80 station test preserved signal, rates and TX failures through `iw station get` and passed two gateway pings with the candidate. | Advertise only capabilities verified for the first USB firmware and hardware. Cover station scan/association, key programming, disconnection and 2.4/5 GHz operation where advertised. Gate AP, monitor, P2P and advanced features on their own evidence. |
| Review and release evidence | The current DKMS results cover one board and one live kernel. The driver has distinct boot, runtime and disconnect paths with firmware-specific confirmation handling. | Document the USB boot and message ABI (versions, lengths, byte order, and command and teardown ownership). Prepare independently buildable patches, run `checkpatch.pl`, GCC/Clang `W=1` and Sparse on the in-tree candidate, and repeat focused lifecycle and capability checks against it. |

## Firmware comparison

This table compares the installed files used by the USB test path with the
vendor's `linux-firmware` files under `aic/aic8800d80/` at `a1ebb5ad7eae`.
The asterisks stand for the shared `8800d80` component of each filename.

| File | Tested USB bytes | `linux-firmware` bytes | Comparison |
| --- | ---: | ---: | --- |
| `fmacfw_*_u02.bin` | 358,072 | 332,536 | Different |
| `fw_adid_*_u02.bin` | 1,708 | 1,708 | Identical SHA-256 |
| `fw_patch_*_u02.bin` | 32,700 | 28,468 | Different |
| `fw_patch_*_u02_ext0.bin` | 16,136 | — | Absent from that directory |
| `fw_patch_table_*_u02.bin` | 1,384 | 23,272 | Different |
| `aic_userconfig_*.txt` | 2,724 | 5,018 | Different |

The [AIC firmware license](https://kernel.googlesource.com/pub/scm/linux/kernel/git/firmware/linux-firmware/+/a1ebb5ad7eae3caca60b8b3db5792cf43339ba58/LICENSES/LICENSE.aic)
permits redistribution of the covered binaries unchanged. Coverage for the
exact unmatched USB binaries needs vendor confirmation. A shared filename
does not establish binary compatibility or license coverage.

## Separate kernel work

The standard `btusb` path passed the Bluetooth checks for **this** `a69c` combo
dongle. The `368b:8d81` [ZLP companion](drivers/aic8800/aic_zlp_quirk/aic_zlp_quirk.c)
uses kprobes. Broadening support to that ID calls for a narrowly scoped,
tested USB/Bluetooth quirk instead. The [generic btusb changes](kernel-patches/bluetooth/README.md)
and [USB-storage installer quirk](kernel-patches/usb-storage/README.md) have
their own review and evidence requirements and can proceed independently.

Start with the USB firmware inventory and the shared-core comparison, then
build a station-only D80 U02 USB branch with native cfg80211 regulatory
handling. That branch is the right place to validate the existing fixes on the
actual patch series reviewers would receive.
