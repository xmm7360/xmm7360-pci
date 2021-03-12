![CI](https://github.com/malikkirchner/xmm7360-pci/workflows/CI/badge.svg)

⚠️ *_In heavy development. No support provided. May not work, may crash your computer, may singe your jaffles._* ⚠️

# Getting started


# What

Driver for Fibocom L850-GL / Intel XMM7360 (PCI ID 8086:7360).

# Status

This release supports native IP.

To test:

- Ensure Python package `ConfigArgParse` is installed for root eg. `sudo pip install --user ConfigArgParse`
- `make && make load`
- If your sim has pin enabled, run `echo "AT+CPIN=\"0000\"" | sudo tee -a /dev/ttyXMM1`. Replace `0000` with your pin code.
- `sudo python3 rpc/open_xdatachannel.py --apn your.apn.here`
- pray (if applicable)

You should receive a `wwan0` interface, with an IP, and a default route.

# Complete example for Ubuntu 20.04

```
mkdir ~/tmp/
cd ~/tmp/
sudo apt install build-essential python3-pyroute2 python3-configargparse git
git clone https://github.com/xmm7360/xmm7360-pci.git
cd xmm7360-pci
make && make load
sudo python3 rpc/open_xdatachannel.py --apn your.apn.here
sudo ip link set wwan0 up
echo "nameserver 1.1.1.1" | sudo tee -a /etc/resolv.conf
echo "nameserver 8.8.8.8" | sudo tee -a /etc/resolv.conf
echo "nameserver 9.9.9.9" | sudo tee -a /etc/resolv.conf
```

# Next

Involvement from someone involved in modem control projects like ModemManager
would be welcome to shape the kernel interfaces so it's not too horrible to
bring up.

Power management support is absent. The modem, as configured, turns off during
suspend, and needs to be reconfigured on resume.
