# What

Driver for Fibocom L850-GL / Intel XMM7360 (PCI ID 8086:7360).

# Status

This release supports PPP connection over AT command ports.

To test:

- ensure `pppd` and Python3 are installed
- edit `test.chat` and replace `telstra.internet` with your own APN
- run `test.sh`

# Next

Native IP networking support is coming shortly.

Involvement from someone involved in modem control projects like ModemManager
would be welcome to shape the kernel interfaces so it's not too horrible to
bring up.
