# Bluetooth USB fixes

Apply these in order against the matching kernel source:

1. `0001-btusb-balance-failed-tx-submissions.patch`: balance a failed counted TX.
2. `0002-btusb-order-unbind-after-hci-close.patch`: use soft unbind so HCI close
   drains RX before USB core disables a present device's endpoints.

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
