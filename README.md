⚠️ *_In heavy development. No support provided. May not work, may crash your computer, may singe your jaffles._* ⚠️

# Getting started


# What

Driver for Fibocom L850-GL / Intel XMM7560 (PCI ID 8086:7560).

# Status

This release supports native IP.

To test:

- Ensure Python package `ConfigArgParse` is installed for root eg. `sudo pip install --user ConfigArgParse`
- `make && make load`
- `sudo python3 rpc/open_xdatachannel.py --apn your.apn.here`
- pray (if applicable)

You should receive a `wwan0` interface, with an IP, and a default route.

# Next

Involvement from someone involved in modem control projects like ModemManager
would be welcome to shape the kernel interfaces so it's not too horrible to
bring up.

Power management support is absent. The modem, as configured, turns off during
suspend, and needs to be reconfigured on resume.
