# D80 USB lifecycle validation

The 21 November 2026 tests used one AIC8800D80 U02 combo dongle with MCU0
and USB IDs `a69c:8d80` in ROM and `a69c:8d81` at runtime. The host ran
`7.3.4-1-cachyos`, NetworkManager 1.58.1 and wpa_supplicant 2.12. Its signed
DKMS modules were built from `485f501` and the reload sequence was from
`d47a467`. The Wi-Fi firmware reported `g586bc1e8`. Signed builds were also
installed for `6.18.60-2-cachyos-lts`. Live results below cover 7.3.4.

A temporary saved NetworkManager profile used a known 2.4 GHz WPA2 network.
Each run established a DHCP lease and passed two IPv4 gateway pings bound to
`wlan0` before the transition. NetworkManager then reactivated the same profile
automatically on the new Wi-Fi interface. The test checked a DHCP lease, two
more interface-bound gateway pings, and continuity of the supplicant process
and nl80211 family ID. The wired default route stayed in place.

| Transition | Time to DHCP | Wi-Fi traffic after return | Bluetooth evidence |
| --- | ---: | --- | --- |
| Reload the two AIC modules with `rmmod` and `modprobe`, keeping `cfg80211` loaded | 6.17 s from starting the unload | 2/2 gateway replies | — |
| Physical unplug and replug into the same USB port | 6.56 s from detecting the hub on replug | 2/2 gateway replies | The same controller returned powered on under `btusb`. |
| Deep system sleep (ACPI S3) and timed RTC wake | 4.36 s from observing completed resume | 2/2 gateway replies | The same powered controller under `btusb` detected two BLE devices in four seconds. |

The physical replug removed the adapter's hub and changed both USB device
numbers. The kernel observed ROM enumeration, firmware upload and runtime
enumeration. The S3 run spent 15.43 seconds asleep and returned with a new USB
radio device number and a new `wlan0` ifindex. Discovery stopped and each
temporary profile was deleted after the check.

An earlier reload using `modprobe -r aic8800_fdrv` also removed `cfg80211`.
The existing supplicant failed to initialize the new interface until restarted.
Removing only the AIC modules preserved the nl80211 family and let the same
supplicant reconnect. The installer uses this sequence.

These are single-cycle smoke tests on one USB revision and AP. Additional
coverage is needed for other revisions and kernels, repeated sleep cycles,
5 GHz or WPA3 reconnection, and Bluetooth pairing and payload traffic.

For provenance, the tested USB Wi-Fi image was `fmacfw_8800d80_u02.bin`
(358,072 bytes, SHA-256
`23f2bed2ac0e8f7bda8f36df7defd30d9b928c97867f6f0f01f28d89dd78d670`).
