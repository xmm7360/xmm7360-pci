#!/usr/bin/env python3

import rpc
import binascii
import time
import sys
import json
import pyroute2

def main(apn,pr):
    r = rpc.XMMRPC()
    ipr = pyroute2.IPRoute()
    ## This should be done every time or only the first time?
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

    r.execute('UtaMsCallPsAttachApnConfigReq', rpc.pack_UtaMsCallPsAttachApnConfigReq(apn), is_async=True)

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

    print("IP assigned: "+str(ip_values))
    print("DNS assigned: "+str(dns_values))

    # use the last nonzero IP
    for addr in ip_values[::-1]:
        if addr.compressed != '0.0.0.0':
            ip_addr = addr.compressed
            break
    
    idx = ipr.link_lookup(ifname='wwan0')
    if len(idx) != 1:
        print("Interface not found")
        return
    idx = idx[0]
    
    ipr.link('set',
            index=idx,
            state='up')
    try:
        ipr.addr('add',
                index=idx,
                address=ip_addr)
    except pyroute2.netlink.exceptions.NetlinkError:
        ## TODO: Check if it is different
        print("Interface already has an IP")

    ipr.route('add',
        dst='default',
        oif=idx,
        priority = int(pr)
        )

    # this gives us way too much stuff, which we need
    pscr = r.execute('UtaMsCallPsConnectReq', rpc.pack_UtaMsCallPsConnectReq(), is_async=True)
    # this gives us a handle we need
    dcr = r.execute('UtaRPCPsConnectToDatachannelReq', rpc.pack_UtaRPCPsConnectToDatachannelReq())

    csr_req = pscr['body'][:-6] + dcr['body'] + b'\x02\x04\0\0\0\0'

    r.execute('UtaRPCPSConnectSetupReq', csr_req)

if __name__ == "__main__":
    with open('config.json') as json_data:
        config = json.load(json_data)
    main(config['apn'],int(config['priority']))
