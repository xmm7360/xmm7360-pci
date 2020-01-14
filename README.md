⚠️ *_In heavy development. No support provided. May not work, may crash your computer, may singe your jaffles._* ⚠️

# Getting started


# What

Driver for Fibocom L850-GL / Intel XMM7360 (PCI ID 8086:7360).

# Status

This release supports native IP.

To test:

- Edit `config.json`; replace `your-apn-here` with your APN
- `make && make load`
- `sudo python3 rpc/open_xdatachannel.py`
- pray (if applicable)

You should receive a `wwan0` interface, with an IP, and a default route.

# Next

Involvement from someone involved in modem control projects like ModemManager
would be welcome to shape the kernel interfaces so it's not too horrible to
bring up.

Power management support is absent. The modem, as configured, turns off during
suspend, and needs to be reconfigured on resume.
