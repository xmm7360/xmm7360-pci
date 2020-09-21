#!/usr/bin/env python3

import logging
# must do this before importing pyroute2
logging.basicConfig(level=logging.DEBUG)

import rpc
import binascii
import time
import sys
from pyroute2 import IPRoute

import configargparse

parser = configargparse.ArgumentParser(
        description='Hacky tool to bring up XMM7x60 modem',
        default_config_files=['./xmm7560.ini', '/etc/xmm7560'],
        )

parser.add_argument('-c', '--conf', is_config_file=True)

parser.add_argument('-a', '--apn', required=True)

parser.add_argument('-n', '--nodefaultroute', action="store_true", help="Don't install modem as default route for IP traffic")
parser.add_argument('-m', '--metric', type=int, default=1000, help="Metric for default route (higher is lower priority)")

parser.add_argument('-r', '--noresolv', action="store_true", help="Don't add modem-provided DNS servers to /etc/resolv.conf")
cfg = parser.parse_args()

r = rpc.XMMRPC()

ipr = IPRoute()

r.execute('UtaMsSmsInit')
r.execute('UtaMsCbsInit')
r.execute('UtaMsNetOpen')
r.execute('UtaMsCallCsInit')
r.execute('UtaMsCallPsInitialize')
r.execute('UtaMsSsInit')
r.execute('UtaMsSimOpenReq')

rpc.do_fcc_unlock(r)
# disable aeroplane mode if had been FCC-locked. first and second args are probably don't-cares
rpc.UtaModeSet(r, 1)

r.execute('UtaMsCallPsAttachApnConfigReq', rpc.pack_UtaMsCallPsAttachApnConfigReq(cfg.apn), is_async=True)

attach = r.execute('UtaMsNetAttachReq', rpc.pack_UtaMsNetAttachReq(), is_async=True)
_, status = rpc.unpack('nn', attach['body'])

if status == 0xffffffff:
    logging.info("Attach failed - waiting to see if we just weren't ready")

    while not r.attach_allowed:
        r.pump()

    attach = r.execute('UtaMsNetAttachReq', rpc.pack_UtaMsNetAttachReq(), is_async=True)
    _, status = rpc.unpack('nn', attach['body'])

    if status == 0xffffffff:
        logging.error("Attach failed again, giving up")
        sys.exit(1)

ip = r.execute('UtaMsCallPsGetNegIpAddrReq', rpc.pack_UtaMsCallPsGetNegIpAddrReq(), is_async=True)
ip_values = rpc.unpack_UtaMsCallPsGetNegIpAddrReq(ip['body'])

dns = r.execute('UtaMsCallPsGetNegotiatedDnsReq', rpc.pack_UtaMsCallPsGetNegotiatedDnsReq(), is_async=True)
dns_values = rpc.unpack_UtaMsCallPsGetNegotiatedDnsReq(dns['body'])

logging.info("IP address: " + ', '.join(map(str, ip_values)))
logging.info("DNS server(s): " + ', '.join(map(str, dns_values['v4'] + dns_values['v6'])))

# For some reason, on IPv6 networks, the GetNegIpAddrReq call returns 8 bytes of the IPv6 address followed by our 4 byte IPv4 address.
# use the last nonzero IP
for addr in ip_values[::-1]:
    if addr.compressed != '0.0.0.0':
        ip_addr = addr.compressed
        break

idx = ipr.link_lookup(ifname='wwan0')[0]

ipr.flush_addr(index=idx)
ipr.link('set',
        index=idx,
        state='up')
ipr.addr('add',
        index=idx,
        address=ip_addr)

if not cfg.nodefaultroute:
    ipr.route('add',
            dst='default',
            priority=cfg.metric,
            oif=idx)

# Add DNS values to /etc/resolv.conf
if not cfg.noresolv:
    with open('/etc/resolv.conf', 'a') as resolv:
        resolv.write('\n# Added by xmm7560\n')
        for dns in dns_values['v4'] + dns_values['v6']:
            resolv.write('nameserver %s\n' % dns)

# this gives us way too much stuff, which we need
pscr = r.execute('UtaMsCallPsConnectReq', rpc.pack_UtaMsCallPsConnectReq(), is_async=True)
# this gives us a handle we need
dcr = r.execute('UtaRPCPsConnectToDatachannelReq', rpc.pack_UtaRPCPsConnectToDatachannelReq())

csr_req = pscr['body'][:-6] + dcr['body'] + b'\x02\x04\0\0\0\0'

r.execute('UtaRPCPSConnectSetupReq', csr_req)
