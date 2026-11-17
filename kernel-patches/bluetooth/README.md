# Bluetooth USB fixes

Apply these in order against the matching kernel source:

1. `0001-btusb-balance-failed-tx-submissions.patch`: balance a failed counted TX.
2. `0002-btusb-order-unbind-after-hci-close.patch`: use soft unbind so HCI close
   drains RX before USB core disables a present device's endpoints.
3. `0003-btusb-free-deferred-tx-packets.patch`: release accepted deferred packets
   on close/resume errors and serialize queue insertion with resume.

`0001-btusb-balance-failed-tx-submissions.patch` fixes the standard kernel
`btusb` driver; it is separate from the AIC Wi-Fi modules.

The counted command/ACL path increments `tx_in_flight` before USB submission.
A rejected submission has no completion callback, so the caller must reverse
that increment under `txlock`. Otherwise a stale count can veto autosuspend.
The patch preserves the returned errno and packet ownership. The shared
submission helper and its uncounted SCO callers are unchanged.

Baseline: CachyOS `cachyos-7.3.4-1`, commit
`779ca9af71579431ca932a13a7c2ed5934b289ad`, `drivers/bluetooth/btusb.c`.
Apply and review this patch against the kernel source you actually build.
It has not been submitted upstream.

Validation on Linux `7.3.4-1-cachyos`:

- Actual-function host regressions with ASan/UBSan covered six submission errors,
  immediate/delayed completion, suspend queueing, uncounted SCO and 1,024 mixed
  concurrent submissions. USB/HCI operations and resource tracking were mocked.
- Pristine and patched `W=1` module builds were warning-free. The pristine source
  version matched the installed module; aliases, parameters and dependencies
  were unchanged by the candidate.
- One RAM-only candidate discovery and original-driver restoration check worked.
  A strict clean-log gate was not met: RX resubmission `-ENOENT` messages appeared
  during both stock and candidate unload. Their exact cause remains unassigned;
  they are outside the changed counted-TX path.

The installed driver was preserved and restored. This is not a live USB-failure,
SCO/audio, autosuspend or system-sleep validation claim.

## RX unload follow-up

The second patch addresses the teardown messages recorded above. A diagnostic
comparison directly captured endpoint shutdown while HCI/RX flags remained set,
followed by rejected resubmission before disconnect. With soft unbind, HCI close
cleared running state and drained receive URBs while endpoints remained enabled;
the fixed unload made no RX resubmission attempt. This changes sequencing, not
the error-logging filters. Physical removal retains USB core's forced flush.

Eight actual-function host regression groups passed with ASan/UBSan. Production
and diagnostic builds were warning-free with `W=1`; diagnostic instrumentation
is absent from these patches. One short AIC hardware comparison completed
diagnostic, fixed and restored-stock discovery, preserving the installed drivers.
The captured journal passed offline replay after a diagnostic-only cancellation
classification correction; raw records were retained. Other vendors, audio and
real system sleep remain unvalidated. Neither patch has been submitted upstream.

## Deferred TX ownership follow-up

A deferred send returns success to HCI core, transferring skb ownership to
`btusb`. Dropping USB-anchor references alone does not free that skb or its
control setup packet. The third patch drains both allocations on close,
failed/closed-HCI resume and deferred submission failure. Successfully submitted
packets remain completion-owned; ordinary negative-return cleanup remains with
HCI core. Deferred anchoring now holds `txlock` across the suspension decision,
so resume cannot drain the queue before a late enqueue strands a packet.

Four old failures were reproduced with extracted actual-function host tests;
eight fixed control groups passed with ASan/UBSan and explicit resource counters.
Production and diagnostic module builds were warning-free with `W=1`.
A diagnostic-only native check exercised the real discard helper on three
synthetic packets without submitting USB traffic. Eight seconds of candidate
discovery and three seconds after stock restoration passed; candidate unload was
clean. Installed source, firmware and module hashes, Wi-Fi/network state and USB
generation were preserved. The diagnostic code is absent from this patch.

This did not force an actual controller resume failure or test system sleep,
audio or other adapters. The current upstream file is
`drivers/bluetooth/btusb_main.c`; the ownership issues were also found in pinned
Torvalds and Bluetooth-next sources. These matching-kernel patches need rebasing
and maintainer review before submission. In particular, the global soft-unbind
change needs broader hardware evidence. None has been submitted or accepted.
