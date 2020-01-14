#!/usr/bin/env python3

import rpc
import binascii
import time
import sys
from pyroute2 import IPRoute

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

r.execute('UtaMsCallPsAttachApnConfigReq', rpc.pack_UtaMsCallPsAttachApnConfigReq("telstra.internet"), is_async=True)

attach = r.execute('UtaMsNetAttachReq', rpc.pack_UtaMsNetAttachReq(), is_async=True)
_, status = rpc.unpack('nn', attach['body'])

if status == 0xffffffff:
    print("Attach failed - waiting to see if we just weren't ready")

    while not r.attach_allowed:
        r.pump()

    attach = r.execute('UtaMsNetAttachReq', rpc.pack_UtaMsNetAttachReq(), is_async=True)
    _, status = rpc.unpack('nn', attach['body'])

    if status == 0xffffffff:
        print("Attach failed again, giving up")
        sys.exit(1)

ip = r.execute('UtaMsCallPsGetNegIpAddrReq', rpc.pack_UtaMsCallPsGetNegIpAddrReq(), is_async=True)
ip_values = rpc.unpack_UtaMsCallPsGetNegIpAddrReq(ip['body'])

dns = r.execute('UtaMsCallPsGetNegotiatedDnsReq', rpc.pack_UtaMsCallPsGetNegotiatedDnsReq(), is_async=True)
dns_values = rpc.unpack_UtaMsCallPsGetNegotiatedDnsReq(dns['body'])

print(ip_values)
print(dns_values)

# For some reason, on IPv6 networks, the GetNegIpAddrReq call returns 8 bytes of the IPv6 address followed by our 4 byte IPv4 address.
# use the last nonzero IP
for addr in ip_values[::-1]:
    if addr.compressed != '0.0.0.0':
        ip_addr = addr.compressed
        break

idx = ipr.link_lookup(ifname='wwan0')[0]
ipr.link('set',
        index=idx,
        state='up')
ipr.addr('add',
        index=idx,
        address=ip_addr)
ipr.route('add',
        dst='default',
        oif=idx)

# Add DNS values to /etc/resolv.conf
with open('/etc/resolv.conf', 'a') as resolv:
    resolv.write('\n# Added by xmm7360\n')
    for dns in dns_values['v4'] + dns_values['v6']:
        resolv.write('nameserver %s\n' % dns)

# this gives us way too much stuff, which we need
pscr = r.execute('UtaMsCallPsConnectReq', rpc.pack_UtaMsCallPsConnectReq(), is_async=True)
# this gives us a handle we need
dcr = r.execute('UtaRPCPsConnectToDatachannelReq', rpc.pack_UtaRPCPsConnectToDatachannelReq())

csr_req = pscr['body'][:-6] + dcr['body'] + b'\x02\x04\0\0\0\0'

r.execute('UtaRPCPSConnectSetupReq', csr_req)
