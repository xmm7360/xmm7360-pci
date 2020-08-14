#!/usr/bin/env python3

import os
import binascii
import time
import struct
import itertools
import ipaddress
import hashlib
import rpc_call_ids
import rpc_unsol_table

def asn_int4(val):
    return b'\x02\x04' + struct.pack('>L', val)

class XMMRPC(object):
    def __init__(self, path='/dev/xmm0/rpc'):
        self.fp = os.open(path, os.O_RDWR | os.O_SYNC)

        # loop over 1..255, excluding 0
        self.tid_gen = itertools.cycle(range(1, 256))

        self.attach_allowed = False

    def pump(self, is_async=False, have_ack=False, tid_word=None):
        message = os.read(self.fp, 131072)
        resp = self.handle_message(message)

        desc = resp['type']

        if resp['type'] == 'unsolicited':
            name = rpc_unsol_table.xmm7360_unsol.get(resp['code'], '0x%02x' % resp['code'])
            desc = 'unsolicited: %s' % name
            self.handle_unsolicited(resp)

        print(desc + ':', format_unknown(resp['body']))
        return resp

    def execute(self, cmd, body=asn_int4(0), is_async=False):
        print("RPC executing %s" % cmd)
        if isinstance(cmd, str):
            cmd = rpc_call_ids.call_ids[cmd]

        if is_async:
            tid = 0x11000101
        else:
            tid = 0

        tid_word = 0x11000100 | tid

        total_length = len(body) + 16
        if tid:
            total_length += 6
        header = struct.pack('<L', total_length) + asn_int4(total_length) + asn_int4(cmd) + struct.pack('>L', tid_word)
        if tid:
            header += asn_int4(tid)

        assert total_length + 4 == len(header) + len(body)

        print(binascii.hexlify(header + body))
        ret = os.write(self.fp, header + body)
        if ret < len(header + body):
            print("write error: %d", ret)

        have_ack = False

        while True:
            resp = self.pump()
            if resp['type'] == 'response':
                break

        return resp

    def handle_message(self, message):
        length = message[:4]
        len1_p = message[4:10]
        code_p = message[10:16]
        txid = message[16:20]
        body = message[20:]

        assert len1_p.startswith(b'\x02\x04')
        assert code_p.startswith(b'\x02\x04')

        l0 = struct.unpack('<L', length)[0]
        l1 = struct.unpack('>L', len1_p[2:])[0]
        code = struct.unpack('>L', code_p[2:])[0]
        txid = struct.unpack('>L', txid)[0]

        if l0 != l1:
            print("length mismatch, framing error?")

        content = unpack_unknown(body)

        if txid == 0x11000100:
            t = 'response'
        elif (txid & 0xffffff00) == 0x11000100:
            if code >= 2000:
                t = 'async_ack'
            else:
                t = 'response'
                assert content[0] == txid
                content = content[1:]
                body = body[6:]
        else:
            t = 'unsolicited'

        return {'tid': txid, 'type': t, 'code': code, 'body': body, 'content': content}

    def handle_unsolicited(self, message):
        name = rpc_unsol_table.xmm7360_unsol.get(message['code'], None)

        if name == 'UtaMsNetIsAttachAllowedIndCb':
            self.attach_allowed = message['content'][2]

def format_unknown(body):
    out = []
    for field in unpack_unknown(body):
        if isinstance(field, int):
            out.append('0x%x' % field)
        else:
            out.append(repr(field))

    return ', '.join(out)


def _pack_string(val, fmt, elem_type):
    length_str = ''
    while len(fmt) and fmt[0].isdigit():
        length_str += fmt.pop(0)

    length = int(length_str)
    assert len(val) <= length
    valid = len(val)

    elem_size = len(struct.pack(elem_type, 0))
    field_type = {1: 0x55, 2: 0x56, 4: 0x57}[elem_size]
    payload = struct.pack('%d%s' % (valid, elem_type), *val)

    count = length * elem_size
    padding = (length - valid) * elem_size

    if valid < 128:
        valid_field = struct.pack('B', valid)
    else:
        remain = valid
        valid_field = [0x80]
        while remain > 0:
            valid_field[0] += 1
            valid_field.insert(1, remain & 0xff)
            remain >>= 8

    field = struct.pack('B', field_type)
    field += bytes(valid_field)
    field += pack('LL', count, padding)
    field += payload
    field += b'\0'*padding
    return field

def take_asn_int(data):
    assert data.pop(0) == 0x02
    l = data.pop(0)
    val = 0
    for i in range(l):
        val <<= 8
        val |= data.pop(0)
    return val

def take_string(data):
    t = data.pop(0)
    assert t in [0x55, 0x56, 0x57]
    valid = data.pop(0)
    if valid & 0x80:    # Variable length!
        value = 0
        for byte in range(valid & 0xf): # lol
            value |= data.pop(0) << (byte*8)
        valid = value
    if t == 0x56:
        valid <<= 1
    elif t == 0x57:
        valid <<= 2
    count = take_asn_int(data)   # often equals valid + padding, but sometimes not
    padding = take_asn_int(data)
    if count:
        assert count == (valid + padding)
        field_size = count
    else:
        field_size = valid
    payload = data[:valid]
    for i in range(valid + padding):
        data.pop(0) # eek
    return payload

def unpack_unknown(data):
    out = []
    data = bytearray(data)

    while len(data):
        t = data[0]
        if t == 0x02:
            out.append(take_asn_int(data))
        elif t in [0x55, 0x56, 0x57]:
            out.append(take_string(data))
        else:
            raise ValueError("unknown type 0x%x" % t)

    return out


def unpack(fmt, data):
    data = bytearray(data)
    out = []
    for ch in fmt:
        if ch == 'n':
            val = take_asn_int(data)
            out.append(val)
        elif ch == 's':
            val = take_string(data)
            out.append(val)
        else:
            raise ValueError("unknown format char %s" % ch)

    return out

def pack(fmt, *args):
    out = b''
    fmt = list(fmt)
    args = list(args)

    while len(fmt):
        arg = args.pop(0)
        ch = fmt.pop(0)

        if ch == 'B':
            out += b'\x02\x01' + struct.pack('B', arg)
        elif ch == 'H':
            out += b'\x02\x02' + struct.pack('>H', arg)
        elif ch == 'L':
            out += b'\x02\x04' + struct.pack('>L', arg)
        elif ch == 's':
            out += _pack_string(arg, fmt, 'B')
        elif ch == 'S':
            elem_type = fmt.pop(0)
            out += _pack_string(arg, fmt, elem_type)
        else:
            raise ValueError("Unknown format char '%s'" % ch)

    if len(args):
        raise ValueError("Too many args supplied")

    return out

def bytes_to_ipv4(data):
    return ipaddress.IPv4Address(int(binascii.hexlify(data), 16))

def bytes_to_ipv6(data):
    return ipaddress.IPv6Address(int(binascii.hexlify(data), 16))

def pack_UtaMsCallPsAttachApnConfigReq(apn):
    apn_string = bytearray(101)
    apn_string[:len(apn)] = apn.encode('ascii')

    args = [0, b'\0'*257, 0, b'\0'*65, b'\0'*65, b'\0'*250, 0, b'\0'*250, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, b'\0'*20, 0, b'\0'*101, b'\0'*257, 0, b'\0'*65, b'\0'*65, b'\0'*250, 0, b'\0'*250, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, b'\0'*20, 0, b'\0'*101, b'\0'*257, 0, b'\0'*65, b'\0'*65, b'\0'*250, 0, b'\0'*250, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0x404, 1, 0, 1, 0, 0, b'\0'*20, 3, apn_string, b'\0'*257, 0, b'\0'*65, b'\0'*65, b'\0'*250, 0, b'\0'*250, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0x404, 1, 0, 1, 0, 0, b'\0'*20, 3, apn_string, 3, 0,]

    types = 'Bs260Ls66s65s250Bs252HLLLLLLLLLLLLLLLLLLLLLs20Ls104s260Ls66s65s250Bs252HLLLLLLLLLLLLLLLLLLLLLs20Ls104s260Ls66s65s250Bs252HLLLLLLLLLLLLLLLLLLLLLs20Ls104s260Ls66s65s250Bs252HLLLLLLLLLLLLLLLLLLLLLs20Ls103BL'
    return pack(types, *args)

def pack_UtaMsNetAttachReq():
    return pack('BLLLLHHLL', 0, 0, 0, 0, 0, 0xffff, 0xffff, 0, 0)

def pack_UtaMsCallPsGetNegIpAddrReq():
    return pack('BLL', 0, 0, 0)

def unpack_UtaMsCallPsGetNegIpAddrReq(data):
    _, addresses, _, _, _, _ = unpack('nsnnnn', data)
    a1 = bytes_to_ipv4(addresses[:4])
    a2 = bytes_to_ipv4(addresses[4:8])
    a3 = bytes_to_ipv4(addresses[8:12])

    return a1, a2, a3


def pack_UtaMsCallPsGetNegotiatedDnsReq():
    return pack('BLL', 0, 0, 0)

def unpack_UtaMsCallPsGetNegotiatedDnsReq(data):
    v4 = []
    v6 = []
    vals = unpack('n' + 'sn'*16 + 'nsnnnn', data)
    for i in range(16):
        address, typ = vals[2*i+1:2*i+3]
        if typ == 1:
            v4.append(bytes_to_ipv4(address[:4]))
        elif typ == 2:
            v6.append(bytes_to_ipv6(address[:16]))

    return {'v4': v4, 'v6': v6}


def pack_UtaMsCallPsConnectReq():
    return pack('BLLL', 0, 6, 0, 0)

def pack_UtaRPCPsConnectToDatachannelReq(path='/sioscc/PCIE/IOSM/IPS/0'):
    bpath = path.encode('ascii') + b'\0'
    return pack('s24', bpath)

def pack_UtaSysGetInfo(index):
    return pack('Ls0L', 0, b'', index)

def unpack_UtaSysGetInfo(body):
    return unpack('nns', body)[-1]

def UtaSysGetInfo(rpc, index):
    resp = rpc.execute('UtaSysGetInfo', pack_UtaSysGetInfo(index))
    return unpack_UtaSysGetInfo(resp['body'])

def UtaModeSet(rpc, mode):
    mode_tid = 15
    resp = rpc.execute('UtaModeSetReq', pack('LLL', 0, mode_tid, mode))
    if resp['content'][0] != 0:
        raise IOError("UtaModeSet failed. Bad value?")

    while True:
        msg = rpc.pump()
        # msg['txid'] will be mode_tid as well
        if rpc_unsol_table.xmm7360_unsol.get(msg['code'], None) == 'UtaModeSetRspCb':
            if msg['content'][0] != mode:
                raise IOError("UtaModeSet was not able to set mode. FCC lock enabled?")
            return

def get_ip(r):
    ip = r.execute('UtaMsCallPsGetNegIpAddrReq', pack_UtaMsCallPsGetNegIpAddrReq(), is_async=True)
    ip_values = unpack_UtaMsCallPsGetNegIpAddrReq(ip['body'])

    dns = r.execute('UtaMsCallPsGetNegotiatedDnsReq', pack_UtaMsCallPsGetNegotiatedDnsReq(), is_async=True)
    dns_values = unpack_UtaMsCallPsGetNegotiatedDnsReq(dns['body'])

    # For some reason, on IPv6 networks, the GetNegIpAddrReq call returns 8 bytes of the IPv6 address followed by our 4 byte IPv4 address.
    # use the last nonzero IP
    for addr in ip_values[::-1]:
        if addr.compressed != '0.0.0.0':
            ip_addr = addr.compressed
            return addr.compressed, dns_values
    return None, None


def do_fcc_unlock(r):
    fcc_status_resp = r.execute('CsiFccLockQueryReq', is_async=True)
    _, fcc_state, fcc_mode = unpack('nnn', fcc_status_resp['body'])
    print("FCC lock: state %d mode %d" % (fcc_state, fcc_mode))
    if not fcc_mode:
        return
    if fcc_state:
        return

    fcc_chal_resp = r.execute('CsiFccLockGenChallengeReq', is_async=True)
    _, fcc_chal = unpack('nn', fcc_chal_resp['body'])
    chal_bytes = struct.pack('<L', fcc_chal)
    # read out from nvm:fix_cat_fcclock.fcclock_hash[0]={0x3D,0xF8,0xC7,0x19}
    key = bytearray([0x3d, 0xf8, 0xc7, 0x19])
    resp_bytes = hashlib.sha256(chal_bytes + key).digest()
    resp = struct.unpack('<L', resp_bytes[:4])[0]
    unlock_resp = r.execute('CsiFccLockVerChallengeReq', pack('L', resp), is_async=True)
    resp = unpack('n', unlock_resp['body'])[0]
    if resp != 1:
        raise IOError("FCC unlock failed")

if __name__ == "__main__":
    rpc = XMMRPC()

    fcc_status = rpc.execute('CsiFccLockQueryReq', is_async=True)
    print("fcc status: %s" % binascii.hexlify(fcc_status['body']))

    rpc.execute('UtaMsSmsInit')
    rpc.execute('UtaMsCbsInit')
    rpc.execute('UtaMsNetOpen')
    rpc.execute('UtaMsCallCsInit')
    rpc.execute('UtaMsCallPsInitialize')
    rpc.execute('UtaMsSsInit')
    rpc.execute('UtaMsSimOpenReq')

    do_fcc_unlock(rpc)
    UtaModeSet(rpc, 1)

    print("Firmware version:", UtaSysGetInfo(rpc, 0))
