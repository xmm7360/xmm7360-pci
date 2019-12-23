#!/usr/bin/env python3

import os
import binascii
import threading
import time
import struct
import itertools

def asn_int4(val):
    return b'\x02\x04' + struct.pack('>L', val)

class XMMRPC(object):
    def __init__(self, path='/dev/xmm0/rpc'):
        self.fp = os.open(path, os.O_RDWR | os.O_SYNC)

        self.callbacks = {}
        self.call_acknowledged = set()
        self.response_event = threading.Event()
        self.response = None
        self.stop = False
        self.reader_thread = threading.Thread(target=self.reader)
        self.reader_thread.start()

        # loop over 1..255, excluding 0
        self.tid_gen = itertools.cycle(range(1, 256))

    def __del__(self):
        self.stop = True
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
        os.write(self.fp, header + body)
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
        while not self.stop:
            dd = os.read(self.fp, 4096)
            self.handle_message(dd)

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
    rpc.stop = True
