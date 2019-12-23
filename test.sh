#!/bin/bash

set -e

make
make load

sudo python rpc/rpc.py &

sudo pppd /dev/ttyXMM2 modem passive defaultroute noipdefault usepeerdns noauth debug nodetach connect "/usr/sbin/chat -v -t15 -f test.chat"
