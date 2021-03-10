#!/bin/bash

# install: sudo ./scripts/lte.sh setup
# use: sudo lte up

CONNECT_APN=your.apn.here # something like telstra.wap
BIN_DIR=/usr/local/bin

if [[ "$1" = "" ]]; then
  echo "nothing to do, try with up, down, setup or remove"
  exit 1
fi

# run as root (Ubuntu GUI Tested)
if [[ $EUID -ne 0 ]]; then
  echo "This script must be run as root, elevating!" 
  exec pkexec $0 $1
  exit 0
fi

# switch to correct directory
SCRIPT_PATH=$(readlink -f $0)
SCRIPT_DIR=`dirname $SCRIPT_PATH`
cd $SCRIPT_DIR/..

echo "lte.sh: manage xmm7360-pci"
echo "APN: $CONNECT_APN"
echo "Script: $SCRIPT_DIR/lte.sh; Link: $BIN_DIR"
echo ""


# install deps, module, and lte script to PATH
if [[ "$1" = "setup" ]]; then
  pip3 install pyroute2 configargparse --user

  unlink /usr/local/bin/lte || true
  ln -s $SCRIPT_DIR/lte.sh /usr/local/bin/lte
  chmod 755 /usr/local/bin/lte


  make
  make load
fi

# remove interface, mod
if [[ "$1" = "remove" ]]; then
  ip link set wwan0 down
  rmmod xmm7360 || true
  unlink /usr/local/bin/lte || true
fi

# down param
if [[ "$1" = "down" ]]; then
  echo "taking wwan0 down!" 
  ip link set wwan0 down
  exit
fi

if [[ "$1" = "up" ]]; then
  echo "bringing wwan0 up!" 

  python3 $SCRIPT_DIR/../rpc/open_xdatachannel.py --apn $CONNECT_APN
  ip link set wwan0 up
fi
