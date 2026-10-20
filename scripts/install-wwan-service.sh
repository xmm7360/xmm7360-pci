#!/bin/bash
# Install wwan.service, an on-demand service for the in-kernel iosm driver (run as root)
# The APN comes from /etc/xmm7360, e.g. `echo apn=your.apn.here > /etc/xmm7360`
set -euo pipefail
here=$(dirname "$(readlink -f "$0")")
repo=$here/..

if ! grep -q '^apn=' /etc/xmm7360 2>/dev/null; then
  echo "no APN configured, create it with: echo apn=your.apn.here > /etc/xmm7360" >&2
  exit 1
fi

# Root runs copies, never user-writable code from the checkout
install -d -m 0755 /usr/local/lib/wwan/rpc
install -m 0644 -t /usr/local/lib/wwan/rpc "$repo"/rpc/{open_xdatachannel,rpc,rpc_call_ids,rpc_unsol_table}.py
install -m 0755 -t /usr/local/lib/wwan "$here"/wwan-up "$here"/wwan-down "$here"/wwan-signal
install -m 0644 -t /etc/systemd/system "$repo"/examples/wwan.service "$repo"/examples/wwan-signal.service
install -m 0644 "$repo"/examples/50-wwan.rules /etc/polkit-1/rules.d/50-wwan.rules
systemctl daemon-reload
systemctl is-active --quiet wwan.service || /usr/local/lib/wwan/wwan-down
echo "installed, connect with: systemctl start wwan"
