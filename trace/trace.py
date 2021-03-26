#!/usr/bin/env python3

import os
import struct
import sys

fd = os.open(sys.argv[1], os.O_RDONLY)


def log(msg):
    if 'shm_sensor' in msg:
        return
    if 'store_metric' in msg:
        return
    if '[ME]' in msg:
        return
    print(msg.strip())


def unescape(packet):
    out = bytearray()
    packet = iter(packet)
    for ch in packet:
        if ch == 0x7d:
            ch = next(packet) | (1 << 5)
        out.append(ch)
    return out


def decode_printf(payload):
    def take_string(payload):
        string, _, payload = payload.partition(b'\0')
        return payload, string.decode('ascii', errors='replace')

    def take_int(payload):
        val = struct.unpack('<L', payload[:4])[0]
        payload = payload[4:]
        return payload, val

    payload, fmt = take_string(payload)

    # print(fmt)

    try:
        where = fmt.find('%')
        args = []
        while where >= 0:
            atype = fmt[where + 1]
            while atype.isdigit() or atype.isspace() or atype in '.l':
                where += 1
                atype = fmt[where + 1]

            # print(atype, ''.join('%02x ' % ch for ch in payload))

            if atype == 's':
                payload, arg = take_string(payload)
                args.append(arg)
            elif atype in 'dxpui':
                payload, arg = take_int(payload)
                args.append(arg)
            else:
                raise ValueError(atype)

            where = fmt.find('%', where + 1)

        fmt = fmt.replace('%p', '0x%x')

        return fmt % tuple(args)

    except Exception as e:
        return 'BAD PRINTF "%s" (%s)' % (fmt, e)


def handle_packet(packet):
    stream, seq = struct.unpack('BB', packet[:2])

    if len(packet) < 13:
        return
    if stream != 0 or packet[7] not in [0x10, 0x11]:
        return

    # print(''.join('%02x ' % ch for ch in packet))

    # strip checksum
    packet = packet[:-5]

    if stream == 0x00 and len(packet) > 8:
        typ = packet[7]
        val = packet[8]
        if val == 0:
            payload = packet[0x9:]
        elif val == 3:
            payload = packet[0xd:]
        else:
            return
        if typ == 0x10:  # print
            log(payload.decode('ascii', errors='replace'))
        if typ == 0x11:  # printf
            log(decode_printf(payload))


buf = b''

while True:
    new = os.read(fd, 1048576)
    if not len(new):
        break
    buf += new
    start = buf.find(b'\x7e')
    if start < 0:
        buf = b''
        continue
    buf = buf[start:]

    while True:
        end = buf.find(b'\x7e', 1)
        if end < 0:
            break
        packet = buf[1:end]
        buf = buf[end:]

        if not len(packet):
            continue

        handle_packet(unescape(packet))
