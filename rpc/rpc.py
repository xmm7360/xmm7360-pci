import os
import binascii
import threading
import time
import struct

fp = os.open('/dev/xmm1', os.O_RDWR | os.O_SYNC)

callbacks = {}
call_acknowledged = set()
response_event = threading.Event()
response = [None]
def _tid_gen():
    counter = 0
    while True:
        counter = (counter + 1) & 0xff
        if counter == 0:
            continue
        yield counter
tid_gen = _tid_gen()

def asn_int4(val):
    return b'\x02\x04' + struct.pack('>L', val)

def send_command(cmd, body=b'', callback=None):
    if callback:
        tid = 0x11000100 | next(tid_gen)
    else:
        tid = 0

    tid_word = 0x11000100 | tid

    if tid:
        callbacks[tid_word] = callback

    total_length = len(body) + 22
    header = struct.pack('<L', total_length) + asn_int4(total_length) + asn_int4(cmd) + struct.pack('>L', tid_word) + asn_int4(tid)

    assert total_length + 4 == len(header) + len(body)

    response_event.clear()
    os.write(fp, header + body)
    response_event.wait()
    return response[0]


def unpack(message):
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
        response_event.set()
        response[0] = (code, body)
    else:
        if txid not in callbacks:
            print('unexpected txid %08x' % txid)
            return

        if txid not in call_acknowledged:
            print('tx %08x acknowledged' % txid)
            call_acknowledged.add(txid)
            response[0] = (code, body)
            response_event.set()
        else:
            print('tx %08x completed' % txid)
            call_acknowledged.remove(txid)
            callbacks[txid](code, body)

def reader():
    while True:
        dd = os.read(fp, 4096)
        unpack(dd)

t = threading.Thread(target=reader)
t.start()

from rpc_unpack_table import rpc_unpack_table
for k, v in rpc_unpack_table.items():
    locals()[v] = k

def callback(code, body):
    print("in callback")
send_command(CsiFccLockQueryReq, callback=callback)

send_command(UtaMsSmsInit)
send_command(UtaMsCbsInit)
send_command(UtaMsNetOpen)
send_command(UtaMsCallCsInit)
send_command(UtaMsCallPsInitialize)
send_command(UtaMsSsInit)
send_command(UtaMsSimOpenReq)
