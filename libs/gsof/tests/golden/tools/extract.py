#!/usr/bin/env python3
"""Pull the DCOL/GSOF byte stream out of the trimble_driver_ros test pcaps."""
import struct, sys, os, glob

def read_pcap(path):
    with open(path, 'rb') as f:
        blob = f.read()
    magic = blob[:4]
    if magic == b'\xd4\xc3\xb2\xa1':
        endian, nano = '<', False
    elif magic == b'\xa1\xb2\xc3\xd4':
        endian, nano = '>', False
    elif magic == b'\x4d\x3c\xb2\xa1':
        endian, nano = '<', True
    else:
        raise ValueError(f'{path}: unknown magic {magic.hex()}')
    linktype = struct.unpack(endian + 'I', blob[20:24])[0]
    off = 24
    packets = []
    while off + 16 <= len(blob):
        ts_s, ts_u, incl, orig = struct.unpack(endian + 'IIII', blob[off:off+16])
        off += 16
        packets.append(blob[off:off+incl])
        off += incl
    return linktype, packets

def payload_of(linktype, pkt):
    if linktype == 1:            # Ethernet
        if len(pkt) < 14: return None
        ethertype = struct.unpack('>H', pkt[12:14])[0]
        rest = pkt[14:]
        while ethertype in (0x8100, 0x88a8):   # VLAN
            ethertype = struct.unpack('>H', rest[2:4])[0]
            rest = rest[4:]
        if ethertype != 0x0800: return None
    elif linktype in (101, 12):  # raw IP
        rest = pkt
    elif linktype == 0:          # loopback / null
        rest = pkt[4:]
    else:
        raise ValueError(f'linktype {linktype}')
    if len(rest) < 20: return None
    ihl = (rest[0] & 0x0F) * 4
    proto = rest[9]
    total = struct.unpack('>H', rest[2:4])[0]
    rest = rest[:total] if total <= len(rest) else rest
    rest = rest[ihl:]
    if proto == 6:               # TCP
        if len(rest) < 20: return None
        doff = (rest[12] >> 4) * 4
        return rest[doff:]
    if proto == 17:              # UDP
        if len(rest) < 8: return None
        length = struct.unpack('>H', rest[4:6])[0]
        return rest[8:length]
    return None

def dcol_packets(stream):
    """Yield (status, type, data) for each valid DCOL packet."""
    i = 0
    while i < len(stream):
        if stream[i] != 0x02:
            i += 1
            continue
        if i + 6 > len(stream):
            break
        status, ptype, length = stream[i+1], stream[i+2], stream[i+3]
        total = length + 6
        if i + total > len(stream):
            break
        if stream[i+total-1] != 0x03:
            i += 1
            continue
        data = stream[i+4:i+4+length]
        csum = (status + ptype + length + sum(data)) & 0xFF
        if csum != stream[i+total-2]:
            i += 1
            continue
        yield status, ptype, data
        i += total

def records(payload):
    """Yield (type, length, body) from a reassembled GSOF payload."""
    i = 0
    while i + 2 <= len(payload):
        rtype, rlen = payload[i], payload[i+1]
        body = payload[i+2:i+2+rlen]
        if len(body) != rlen:
            yield ('SHORT', rtype, rlen, body)
            return
        yield (rtype, rlen, body)
        i += 2 + rlen

def main():
    for path in sorted(glob.glob(sys.argv[1] + '/*.pcap')):
        linktype, packets = read_pcap(path)
        stream = b''.join(p for p in (payload_of(linktype, pk) for pk in packets) if p)
        print(f'=== {os.path.basename(path)}  linktype={linktype} packets={len(packets)} stream={len(stream)}B')
        pages = {}
        for status, ptype, data in dcol_packets(stream):
            if ptype != 0x40:
                print(f'    packet type 0x{ptype:02x} status 0x{status:02x} len {len(data)}')
                continue
            tx, pg, mx = data[0], data[1], data[2]
            pages.setdefault(tx, bytearray())
            pages[tx] += data[3:]
            if pg >= mx:
                for rec in records(bytes(pages[tx])):
                    if rec[0] == 'SHORT':
                        print(f'    !! short record type {rec[1]} declared {rec[2]} got {len(rec[3])}')
                    else:
                        rtype, rlen, body = rec
                        print(f'    tx={tx} rec {rtype:3d} len {rlen:3d}  {body.hex()}')
                pages.pop(tx)

if __name__ == '__main__':
    main()
