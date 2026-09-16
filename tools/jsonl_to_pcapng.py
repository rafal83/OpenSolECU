"""Convert OpenSolECU JSONL archives to Wireshark PCAPNG (802.15.4 without FCS)."""
import argparse
import json
import pathlib
import struct


def padded(data):
    return data + b'\0' * (-len(data) % 4)


def option(code, data):
    return struct.pack('<HH', code, len(data)) + padded(data)


def block(kind, data):
    data = padded(data)
    size = len(data) + 12
    return struct.pack('<II', kind, size) + data + struct.pack('<I', size)


def convert(source, destination):
    count = 0
    with source.open(encoding='utf-8-sig') as inp, destination.open('wb') as out:
        out.write(block(0x0a0d0d0a, struct.pack('<IHHQ', 0x1a2b3c4d, 1, 0, 0xffffffffffffffff)))
        for channel in range(11, 27):
            name = f'OpenSolECU channel {channel}'.encode()
            out.write(block(1, struct.pack('<HHI', 230, 0, 125) + option(2, name) + option(9, b'\x06') + option(0, b'')))
        for line in inp:
            if not line.strip():
                continue
            row = json.loads(line)
            raw = bytes.fromhex(row['hex'])
            channel = row['channel']
            if not 11 <= channel <= 26 or len(raw) > 125 or len(raw) != row['length']:
                raise ValueError(f'Invalid frame {row.get("id")}: length/channel')
            epoch = row.get('timestampUs', 0)
            timestamp = int(epoch or row['monotonicUs'])
            comment = (f'channel={channel} RSSI={row.get("rssi")} dBm LQI={row.get("lqi")} '
                       f'OpenSolECU id={row.get("id")} timestamp={"UTC" if epoch else "unsynchronized"} FCS omitted').encode()
            header = struct.pack('<IIIII', channel - 11, timestamp >> 32, timestamp & 0xffffffff, len(raw), len(raw))
            out.write(block(6, header + padded(raw) + option(1, comment) + option(0, b'')))
            count += 1
    return count


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=pathlib.Path)
    parser.add_argument('destination', type=pathlib.Path)
    args = parser.parse_args()
    if args.source.resolve() == args.destination.resolve():
        parser.error('Source and destination must differ')
    print('Converted', convert(args.source, args.destination), 'frames')
