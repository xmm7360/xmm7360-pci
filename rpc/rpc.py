#!/usr/bin/env python3

import os
import binascii
import threading
import time
import struct
import itertools
import signal

def asn_int4(val):
    return b'\x02\x04' + struct.pack('>L', val)

class XMMRPC(object):
    def __init__(self, path='/dev/xmm0/rpc'):
        self.fp = os.open(path, os.O_RDWR | os.O_SYNC)

        self.callbacks = {}
        self.call_acknowledged = set()
        self.response_event = threading.Event()
        self.response = None
        self._stop = False
        self.reader_thread = threading.Thread(target=self.reader)
        self.reader_thread.start()

        # loop over 1..255, excluding 0
        self.tid_gen = itertools.cycle(range(1, 256))

    def __del__(self):
        self.stop()
        os.close(self.fp)

    def execute(self, cmd, body=b'', callback=None):
        if callback:
            tid = 0x11000100 | next(self.tid_gen)
        else:
            tid = 0

        tid_word = 0x11000100 | tid

        if tid:
            self.callbacks[tid_word] = callback

        total_length = len(body) + 22
        header = struct.pack('<L', total_length) + asn_int4(total_length) + asn_int4(cmd) + struct.pack('>L', tid_word) + asn_int4(tid)

        assert total_length + 4 == len(header) + len(body)

        self.response_event.clear()
        print(binascii.hexlify(header + body))
        ret = os.write(self.fp, header + body)
        if ret < len(header + body):
            print("write error: %d", ret)
        self.response_event.wait()
        return self.response

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

        if txid == 0:
            print('unsolicited: %04x: %s' % (code, binascii.hexlify(body)))
        elif txid == 0x11000100:
            print('%04x: %s' % (code, binascii.hexlify(body)))
            self.response_event.set()
            self.response = (code, body)
        else:
            if txid not in self.callbacks:
                print('unexpected txid %08x' % txid)
                return

            if txid not in self.call_acknowledged:
                print('tx %08x acknowledged' % txid)
                self.call_acknowledged.add(txid)
                self.response = (code, body)
                self.response_event.set()
            else:
                print('tx %08x completed' % txid)
                self.call_acknowledged.remove(txid)
                cb_thread = threading.Thread(target=self.callbacks[txid], args=(code, body))
                cb_thread.start()
                self.callbacks.pop(txid)

    def reader(self):
        while not self._stop:
            dd = os.read(self.fp, 32768)
            self.handle_message(dd)

    def stop(self):
        self._stop = True
        # interrupt os.read()
        os.kill(os.getpid(), signal.SIGALRM)

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

def pack_UtaMsCallPsConnectReq():
    return pack('BLLL', 0, 6, 0, 0)

def pack_UtaRPCPsConnectToDatachannelReq(path='/sioscc/PCIE/IOSM/IPS/0'):
    bpath = path.encode('ascii') + b'\0'
    return pack('s24', bpath)



if __name__ == "__main__":
    rpc = XMMRPC()

    from rpc_unpack_table import rpc_unpack_table
    for k, v in rpc_unpack_table.items():
        locals()[v] = k

    def callback(code, body):
        print("in callback")
        stop = True
    rpc.execute(CsiFccLockQueryReq, callback=callback)

    rpc.execute(UtaMsSmsInit)
    rpc.execute(UtaMsCbsInit)
    rpc.execute(UtaMsNetOpen)
    rpc.execute(UtaMsCallCsInit)
    rpc.execute(UtaMsCallPsInitialize)
    rpc.execute(UtaMsSsInit)
    rpc.execute(UtaMsSimOpenReq)
    rpc.stop()
