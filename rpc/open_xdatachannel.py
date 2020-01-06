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
r.execute(UtaMsCallPsConnectReq, rpc.pack_UtaMsCallPsConnectReq(), is_async=True)
dcr = r.execute(UtaRPCPsConnectToDatachannelReq, rpc.pack_UtaRPCPsConnectToDatachannelReq())
print("dcr req done", binascii.hexlify(dcr[1]))


csr = bytearray(binascii.unhexlify('020100020400000000020400000000020400000000020100020100020100020100020100020100020100020100020100020100020100020100020100020100020100020100020100020100020100020100020100020100020100020100020100020400000000551402040000001702040000000300000000000000000000000000000000000000000000000204FFFFFFFF0204FFFFFFFF0204000000010204000000030204000000050201000201FA0201000201FA0201000201000201FF0201FF02010202010A0201030201070201FE0201FE02019602010302010202010302010102011F0201090201020201040201020201050204000000055514020400000014020400000000EC42632F8411A11D0AD5862E0000000000000000020400000000020400000000020400020016020400000000'))
# XXX also contains the ConnectToDatachannelReq handle
csr[264:264+12] = ips
r.execute(UtaRPCPSConnectSetupReq, csr)

r.stop()
