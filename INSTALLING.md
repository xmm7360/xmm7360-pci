# Installing

## Dependencies

- build-essential
- python3-pyroute2
- python3-configargparse

## Ubuntu 20.04

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
