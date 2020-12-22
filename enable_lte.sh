#! /bin/bash
APN=your.apn.here # something like gprs.swisscom.ch

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
cd $DIR

make load
sudo python3 rpc/open_xdatachannel.py --apn $APN
sudo ip link set wwan0 up
echo "nameserver 1.1.1.1" | sudo tee -a /etc/resolv.conf
echo "nameserver 8.8.8.8" | sudo tee -a /etc/resolv.conf
echo "nameserver 9.9.9.9" | sudo tee -a /etc/resolv.conf
