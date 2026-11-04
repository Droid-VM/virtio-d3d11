"""Index exact single-tile AV1 bytes using IVF and matching FFmpeg trace_headers.
Packet timestamp is container time, NOT the presentation time of every hidden frame.
Unsupported multi-tile or split frame-header/tile-group input is rejected.
"""
import argparse
import hashlib
import json
import re
import struct
from pathlib import Path

def packets(data):
    if len(data) < 32 or data[:4] != b'DKIF' or data[8:12] != b'AV01':
        raise ValueError('Expected AV1 IVF')
    if struct.unpack_from('<H', data, 6)[0] != 32:
        raise ValueError('Unsupported IVF header length')
    rate, scale = struct.unpack_from('<II', data, 16)
    if not rate or not scale:
        raise ValueError('Invalid IVF time base')
    pos = 32
    number = 0
    while pos < len(data):
        if len(data)-pos < 12:
            raise ValueError('Truncated IVF packet header')
        size, timestamp = struct.unpack_from('<IQ', data, pos)
        pos += 12
        if size > len(data)-pos:
            raise ValueError('Truncated IVF packet')
        yield number, timestamp, timestamp*scale/rate, data[pos:pos+size]
        pos += size
        number += 1

def obus(data):
    pos = 0
    while pos < len(data):
        start = pos
        header = data[pos]
        pos += 1
        if header & 0x81 or not header & 2:
            raise ValueError('Invalid/unsized OBU header')
        if header & 4:
            if pos >= len(data):
                raise ValueError('Truncated extension')
            pos += 1
        size = 0
        for i in range(8):
            if pos >= len(data):
                raise ValueError('Truncated LEB128')
            byte = data[pos]
            pos += 1
            size |= (byte & 127) << (7*i)
            if byte < 128:
                break
        else:
            raise ValueError('Overlong LEB128')
        end = pos+size
        if end > len(data):
            raise ValueError('Truncated OBU')
        yield (header >> 3) & 15, start, pos, end
        pos = end

def trace_frames(lines):
    result = []
    packet = -1
    current = None
    section = None
    field = re.compile(r'\]\s+(\d+)\s+(\S+)\s+([01]+) = (-?\d+)')
    for line in lines:
        if '[trace_headers' not in line:
            continue
        if '] Packet:' in line:
            packet += 1
            current = None
            section = None
        if '] OBU header' in line:
            section = None
        if '] Frame Header' in line:
            current = {'packet': packet, 'fields': {}, 'end_bits': None, 'tiles': None, 'group': False}
            result.append(current)
            section = 'frame'
        if '] Tile Group' in line:
            if current is None or section != 'frame':
                raise ValueError('Unassociated tile group')
            current['group'] = True
            section = 'tile'
        if section == 'frame':
            m = field.search(line)
            if m:
                offset, name, bits, value = m.groups()
                current['fields'][name] = int(value)
                current['end_bits'] = int(offset)+len(bits)
            m = re.search(r'\btiles (\d+)x(\d+)\.', line)
            if m:
                current['tiles'] = [int(m[1]), int(m[2])]
    return result

def build_index(ivf, trace):
    frames = trace_frames(trace.splitlines())
    index = []
    next_frame = 0
    for packet, timestamp, seconds, data in packets(ivf):
        for kind, start, payload, end in obus(data):
            if kind == 4:
                raise ValueError('Split tile-group OBU unsupported')
            if kind not in (3, 6):
                continue
            if next_frame >= len(frames):
                raise ValueError('Trace missing frame header')
            frame = frames[next_frame]
            next_frame += 1
            if frame['packet'] != packet:
                raise ValueError('Trace packet/frame order mismatch')
            fields = frame['fields']
            if 'show_existing_frame' not in fields:
                raise ValueError('Trace missing show_existing_frame')
            if fields['show_existing_frame']:
                if kind != 3 or frame['group']:
                    raise ValueError('Unexpected show-existing layout')
                continue
            if kind != 6 or frame['tiles'] != [1, 1] or not frame['group']:
                raise ValueError('Only combined single-tile FRAME_OBU supported')
            bits = frame['end_bits']
            if bits is None or bits % 8:
                raise ValueError('Trace does not end on byte alignment')
            tile_start = start+bits//8
            if not payload <= tile_start < end:
                raise ValueError('Tile boundary outside frame OBU')
            tile = data[tile_start:end]
            index.append({'coded_index': len(index), 'packet': packet,
                'packet_timestamp': timestamp, 'packet_time_seconds': seconds,
                'frame_type': fields.get('frame_type'), 'show_frame': fields.get('show_frame'),
                'order_hint': fields.get('order_hint'), 'tile': 0, 'tiles': 1,
                'row': 0, 'col': 0, 'bytes': len(tile),
                'sha256': hashlib.sha256(tile).hexdigest()})
    if next_frame != len(frames):
        raise ValueError('Unused trace frames; inputs do not correspond')
    return {'schema': 1, 'ivf_sha256': hashlib.sha256(ivf).hexdigest(),
        'trace_sha256': hashlib.sha256(trace.encode()).hexdigest(), 'entries': index,
        'scope': 'Exact source single-tile identity; packet time is not per-frame display PTS.'}

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('ivf', type=Path)
    p.add_argument('trace', type=Path)
    p.add_argument('output', type=Path)
    args = p.parse_args()
    result = build_index(args.ivf.read_bytes(), args.trace.read_text(encoding='utf-8', errors='replace'))
    args.output.write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print('INDEX_OK coded_tiles='+str(len(result['entries'])))

if __name__ == '__main__':
    main()
