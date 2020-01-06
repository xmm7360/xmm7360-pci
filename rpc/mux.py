#!/usr/bin/env python

import os
import binascii
import threading
import time
import struct
import itertools
import threading
import pytap2

import sys
sys.path.append('../../vfio_logs')
from parse_queue_0 import parse_tags

class MuxPacket(object):
    def __init__(self, seq=0):
        self.seq = seq
        self.packet = b''
        self.fwd_pointer = None

    def get_packet(self):
        # put the final length in
        self.packet = self.packet[:8] + struct.pack('<L', len(self.packet)) + self.packet[8+4:]
        return self.packet

    def append_tag(self, tag, data=b'', extra=0):
        hdr = tag

        if self.packet == b'':  # first tag gets a sequence number
            hdr += struct.pack('<HH', 0, self.seq)
        else:
            while len(self.packet) & 3:
                self.packet += b'\0'
            self.packet = self.packet[:self.fwd_pointer] + struct.pack('<L', len(self.packet)) + self.packet[self.fwd_pointer+4:]

        hdr_len = len(hdr) + 8
        hdr += struct.pack('<HHL', hdr_len+len(data), extra, 0)
        # that last field is the next-tag pointer
        self.fwd_pointer = len(self.packet) + len(hdr) - 4

        self.packet += hdr + data

class XMMMux(object):
    def package(self, packet_data):
        self.seq = (self.seq + 1) & 0xff
        # sub_len = 16 + len(packet_data)
        # pad = b''
        # while sub_len & 3:
        #     pad += b'\0'
        #     sub_len += 1
        # # ADTH: adth len, qlth pointer, ?, body len, adth body
        # tail = pad
        # tail += b'ADTH' + struct.pack('<LLLLL', 0x18, sub_len+0x18, 0, 0x10, len(packet_data))
        # tail += b'QLTH' + struct.pack('<L', 20) + b'\0'*12
        # total_len = sub_len + len(tail)
        # header = b'ADBH' + struct.pack('<HHLL', 0, self.seq, total_len, sub_len)
        # pkt = header + packet_data + tail
        p = MuxPacket(seq=self.seq)
        packet_data = b'\0' * 16 + packet_data
        p.append_tag(b'ADBH', packet_data)
        p.append_tag(b'ADTH', struct.pack('<LLL', 0, 0x10, len(packet_data)))
        # p.append_tag(b'QLTH', b'\0'*12)
        pkt = p.get_packet()
        # print(parse_tags(pkt))
        return pkt

    def tun_thread(self):
        self.tun = pytap2.TapDevice(pytap2.TapMode.Tun)
        self.tun.up()

        while True:
            packet = self.tun.read()

            # print('tun', binascii.hexlify(packet))
            os.write(self.fp, self.package(packet))

    def __init__(self, path='/dev/xmm0/mux'):
        self.fp = os.open(path, os.O_RDWR | os.O_SYNC)

        os.write(self.fp, binascii.unhexlify('41434248000000002C00000010000000434D44481C0000000000000001000000000000000000000000000000'))

        self.thread = threading.Thread(target=self.tun_thread)
        self.thread.start()

        self.seq = 0

        raw = binascii.unhexlify('6000000000383AFFFE800000000000000000000000000001FE80000000000000D438C1FD077C00C38600248840005550000000000000000005010000000005DC03044040FFFFFFFFFFFFFFFF0000000020018004142021F50000000000000000')
        pak = self.package(raw)
        pkd = binascii.unhexlify('414442480000010088000000700000006000000000383AFFFE800000000000000000000000000001FE80000000000000D438C1FD077C00C38600248840005550000000000000000005010000000005DC03044040FFFFFFFFFFFFFFFF0000000020018004142021F50000000000000000414454481800000000000000000000001000000060000000')
        p = MuxPacket(seq=1)
        p.append_tag(b'ADBH', binascii.unhexlify('6000000000383AFFFE800000000000000000000000000001FE80000000000000D438C1FD077C00C38600248840005550000000000000000005010000000005DC03044040FFFFFFFFFFFFFFFF0000000020018004142021F50000000000000000'))
        p.append_tag(b'ADTH', struct.pack('<LLL', 0, 0x10, 0x60))

        print(binascii.hexlify(p.get_packet()))
        print(binascii.hexlify(pkd))

        assert pkd == p.get_packet()

        # time.sleep(1)
        os.write(self.fp, self.package(binascii.unhexlify('6000000000203AFFFE80000000000000D438C1FD077C00C3FE800000000000001645C407D3DA70018700DC1900000000FE800000000000001645C407D3DA70010101B095B5555F54')))

        while True:
            data = os.read(self.fp, 8192)
            # print('air', data)
            if data.startswith(b'ADBH'):
                unq, seq, length, sublen = struct.unpack('<HHLL', data[4:16])

                tail = data[sublen:]

                if tail.startswith(b'ADTH'):
                    length, unk = struct.unpack('<HH', tail[4:8])
                    tail = tail[16:]
                    while len(tail) >= 8:
                        offset, length = struct.unpack('<LL', tail[:8])
                        puk = data[offset:offset+length]
                        # print('ai>', binascii.hexlify(puk))
                        self.tun.write(puk)
                        tail = tail[8:]




if __name__ == "__main__":
    mux = XMMMux()
