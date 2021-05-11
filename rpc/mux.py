#!/usr/bin/env python

import os
import binascii
import struct
import pytap2
import selectors


class MuxPacket(object):
    def __init__(self, seq=0):
        self.seq = seq
        self.packet = b''
        self.fwd_pointer = None

    def get_packet(self):
        # put the final length in
        self.packet = self.packet[:8] + \
            struct.pack('<L', len(self.packet)) + self.packet[8 + 4:]
        return self.packet

    def append_tag(self, tag, data=b'', extra=0):
        hdr = tag

        if self.packet == b'':  # first tag gets a sequence number
            hdr += struct.pack('<HH', 0, self.seq)
        else:
            while len(self.packet) & 3:
                self.packet += b'\0'
            self.packet = self.packet[:self.fwd_pointer] + struct.pack(
                '<L', len(self.packet)) + self.packet[self.fwd_pointer + 4:]

        hdr_len = len(hdr) + 8
        hdr += struct.pack('<HHL', hdr_len + len(data), extra, 0)
        # that last field is the next-tag pointer
        self.fwd_pointer = len(self.packet) + len(hdr) - 4

        self.packet += hdr + data


class XMMMux(object):
    def package(self, packet_data):
        self.seq = (self.seq + 1) & 0xff
        p = MuxPacket(seq=self.seq)
        packet_data = b'\0' * 16 + packet_data
        p.append_tag(b'ADBH', packet_data)
        p.append_tag(b'ADTH', struct.pack('<LLL', 0, 0x10, len(packet_data)))
        # p.append_tag(b'QLTH', b'\0'*12)
        pkt = p.get_packet()
        return pkt

    def __init__(self, path='/dev/xmm0/mux'):
        self.fp = os.open(path, os.O_RDWR | os.O_SYNC)

        self.seq = 0

        pkd = binascii.unhexlify('414442480000010088000000700000006000000000383AFFFE800000000000000000000000000001FE80000000000000D438C1FD077C00C38600248840005550000000000000000005010000000005DC03044040FFFFFFFFFFFFFFFF0000000020018004142021F50000000000000000414454481800000000000000000000001000000060000000')
        p = MuxPacket(seq=1)
        p.append_tag(b'ADBH', binascii.unhexlify(
            '6000000000383AFFFE800000000000000000000000000001FE80000000000000D438C1FD077C00C38600248840005550000000000000000005010000000005DC03044040FFFFFFFFFFFFFFFF0000000020018004142021F50000000000000000'))
        p.append_tag(b'ADTH', struct.pack('<LLL', 0, 0x10, 0x60))

        print(binascii.hexlify(p.get_packet()))
        print(binascii.hexlify(pkd))

        assert pkd == p.get_packet()

        self.tun = pytap2.TapDevice(pytap2.TapMode.Tun)
        self.tun.up()

        sel = selectors.DefaultSelector()
        sel.register(self.fp, selectors.EVENT_READ, self.read_mux)
        sel.register(self.tun, selectors.EVENT_READ, self.read_tun)

        p = MuxPacket()
        p.append_tag(b'ACBH')
        p.append_tag(b'CMDH', struct.pack('<LLLL', 1, 0, 0, 0))
        os.write(self.fp, p.get_packet())

        while True:
            events = sel.select()
            for key, mask in events:
                callback = key.data
                callback()

    def read_tun(self):
        packet = self.tun.read()
        os.write(self.fp, self.package(packet))

    def read_mux(self):
        data = os.read(self.fp, 8192)
        if data.startswith(b'ADBH'):
            unq, seq, length, sublen = struct.unpack('<HHLL', data[4:16])

            tail = data[sublen:]

            if tail.startswith(b'ADTH'):
                length, unk = struct.unpack('<HH', tail[4:8])
                tail = tail[16:]
                while len(tail) >= 8:
                    offset, length = struct.unpack('<LL', tail[:8])
                    puk = data[offset:offset + length]
                    # print('ai>', binascii.hexlify(puk))
                    self.tun.write(puk)
                    tail = tail[8:]


if __name__ == "__main__":
    mux = XMMMux()
