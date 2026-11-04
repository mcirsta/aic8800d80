#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Install only the model policy. DESTDIR supports package/image staging.
set -eu
policy_src=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
policy_root=${DESTDIR:-}
[ -x /usr/bin/python3 ] || { echo "Python 3 at /usr/bin/python3 is required" >&2; exit 1; }
install -d -m 755 "$policy_root/usr/libexec" "$policy_root/usr/lib/udev/rules.d"
install -m 755 "$policy_src/aic8800-installer-policy" "$policy_root/usr/libexec/aic8800-installer-policy"
install -m 644 "$policy_src/59-aic8800-installer.rules" "$policy_root/usr/lib/udev/rules.d/59-aic8800-installer.rules"
# Reloading/triggering is deliberately left to the package manager/caller.
