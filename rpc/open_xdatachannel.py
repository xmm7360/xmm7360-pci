#!/usr/bin/env python3

import rpc
import binascii
import time
r = rpc.XMMRPC()

from rpc_unpack_table import rpc_unpack_table
for k, v in rpc_unpack_table.items():
    locals()[v] = k

r.execute(UtaMsSmsInit)
r.execute(UtaMsCbsInit)
r.execute(UtaMsNetOpen)
r.execute(UtaMsCallCsInit)
r.execute(UtaMsCallPsInitialize)
r.execute(UtaMsSsInit)
r.execute(UtaMsSimOpenReq)
r.execute(UtaMsCallPsAttachApnConfigReq, rpc.pack_UtaMsCallPsAttachApnConfigReq("telstra.internet"), is_async=True)
r.execute(UtaMsNetAttachReq, rpc.pack_UtaMsNetAttachReq(), is_async=True)
ip = r.execute(UtaMsCallPsGetNegIpAddrReq, rpc.pack_UtaMsCallPsGetNegIpAddrReq(), is_async=True)
print(rpc.unpack_UtaMsCallPsGetNegIpAddrReq(ip[1]))
dns = r.execute(UtaMsCallPsGetNegotiatedDnsReq, rpc.pack_UtaMsCallPsGetNegotiatedDnsReq(), is_async=True)
print(rpc.unpack_UtaMsCallPsGetNegotiatedDnsReq(dns[1]))

# this gives us way too much stuff, which we need
pscr = r.execute(UtaMsCallPsConnectReq, rpc.pack_UtaMsCallPsConnectReq(), is_async=True)
# this gives us a handle we need
dcr = r.execute(UtaRPCPsConnectToDatachannelReq, rpc.pack_UtaRPCPsConnectToDatachannelReq())

csr_req = pscr[1][:-6] + dcr[1] + b'\x02\x04\0\0\0\0'

r.execute(UtaRPCPSConnectSetupReq, csr_req)

r.stop()
