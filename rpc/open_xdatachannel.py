#!/usr/bin/env python3

import rpc
import binascii
import time
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
r.execute('UtaModeSetReq', rpc.pack('LLL', 0, 15, 1))

r.execute('UtaMsCallPsAttachApnConfigReq', rpc.pack_UtaMsCallPsAttachApnConfigReq("telstra.internet"), is_async=True)
r.execute('UtaMsNetAttachReq', rpc.pack_UtaMsNetAttachReq(), is_async=True)
ip = r.execute('UtaMsCallPsGetNegIpAddrReq', rpc.pack_UtaMsCallPsGetNegIpAddrReq(), is_async=True)
ip_values = rpc.unpack_UtaMsCallPsGetNegIpAddrReq(ip['body'])

dns = r.execute('UtaMsCallPsGetNegotiatedDnsReq', rpc.pack_UtaMsCallPsGetNegotiatedDnsReq(), is_async=True)
dns_values = rpc.unpack_UtaMsCallPsGetNegotiatedDnsReq(dns['body'])

print(ip_values)
print(dns_values)

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

# this gives us way too much stuff, which we need
pscr = r.execute('UtaMsCallPsConnectReq', rpc.pack_UtaMsCallPsConnectReq(), is_async=True)
# this gives us a handle we need
dcr = r.execute('UtaRPCPsConnectToDatachannelReq', rpc.pack_UtaRPCPsConnectToDatachannelReq())

csr_req = pscr['body'][:-6] + dcr['body'] + b'\x02\x04\0\0\0\0'

r.execute('UtaRPCPSConnectSetupReq', csr_req)
