#!/usr/bin/env python3
"""Read Ethernet pcaps; measure RGB identity changes separately from sync cadence.

Usage: python3 tools/colorlight_capture_audit.py CONFIG INPUT.pcap OUTPUT.pcap REPORT_PREFIX [PORT]
No traffic is generated. Writes a JSON summary and a per-refresh CSV.
Use a single established compressed NDSCPP connection; PORT defaults to 49153.
Capture input at least two seconds before output to cover buffered frames.
"""
import bisect
import collections
import contextlib
import csv
import hashlib
import json
import pathlib
import statistics
import struct
import sys
import zlib


def packets(path):
    with (contextlib.nullcontext(sys.stdin.buffer) if str(path) == '-' else open(path, 'rb')) as f:
        header = f.read(24)
        formats = {b'\xd4\xc3\xb2\xa1': ('<', 1_000_000), b'\xa1\xb2\xc3\xd4': ('>', 1_000_000),
                   b'\x4d\x3c\xb2\xa1': ('<', 1_000_000_000), b'\xa1\xb2\x3c\x4d': ('>', 1_000_000_000)}
        endian, scale = formats[header[:4]]
        assert struct.unpack_from(endian + 'I', header, 20)[0] == 1, 'Ethernet pcap required'
        while h := f.read(16):
            assert len(h) == 16, 'Truncated pcap record'
            sec, fraction, captured, original = struct.unpack(endian + '4I', h)
            data = f.read(captured)
            assert len(data) == captured == original, 'Truncated captured packet'
            yield sec * 1_000_000_000 + fraction * (1_000_000_000 // scale), data


def intervals(times):
    values = [b - a for a, b in zip(times, times[1:])]
    if not values:
        return {'intervals': 0}
    ordered = sorted(values)
    return {'intervals': len(values), 'rate_hz': len(values) * 1e9 / (times[-1] - times[0]),
            'min_ms': min(values) / 1e6, 'median_ms': statistics.median(values) / 1e6,
            'mean_ms': statistics.mean(values) / 1e6, 'p99_ms': ordered[int((len(ordered) - 1) * .99)] / 1e6,
            'max_ms': max(values) / 1e6, 'stddev_ms': statistics.pstdev(values) / 1e6}


def identity(rgb):
    # Planar representation avoids slow Python loops over individual pixels.
    return hashlib.sha256(rgb[0::3] + rgb[1::3] + rgb[2::3]).hexdigest()


def audit(config, input_path, output_path, prefix, input_port=49153):
    width, height = config['width'], config['height']
    output = config['output']
    segments = {}
    statuses = []
    for time_ns, b in packets(input_path):
        if b[12:14] != b'\x08\x00':
            continue
        ip = 14
        if b[ip + 9] != 6:
            continue
        ihl = (b[ip] & 15) * 4
        tcp = ip + ihl
        source, destination, seq = struct.unpack_from('>HHI', b, tcp)
        tcp_length = (b[tcp + 12] >> 4) * 4
        size = struct.unpack_from('>H', b, ip + 2)[0] - ihl - tcp_length
        payload = b[tcp + tcp_length:tcp + tcp_length + size]
        if source == input_port and len(payload) == 64 and struct.unpack_from('<I', payload)[0] == 64:
            status = struct.unpack('<II5d4I', payload)
            statuses.append((status[8], status[4]))  # queue depth, newest-frame lead
        if destination == input_port and size > 0:
            previous = segments.get(seq)
            if previous is None or len(payload) > len(previous[0]):
                segments[seq] = (payload, time_ns)
    stream = bytearray()
    ends, arrival_times = [], []
    last = None
    for seq, (payload, time_ns) in sorted(segments.items()):
        assert last is None or seq <= last, 'TCP capture has a sequence gap'
        skip = max(0, (last or seq) - seq)
        stream.extend(payload[skip:])
        last = max(last or seq, seq + len(payload))
        ends.append(len(stream))
        arrival_times.append(time_ns)
    # Capture begins in an established stream. Find a validated frame boundary.
    start = stream.find(b'EVAD')
    incoming = []
    while start >= 0 and start + 24 <= len(stream):
        compressed = stream[start:start + 4] == b'EVAD'
        size = struct.unpack_from('<I', stream, start + 4)[0]
        length = 16 + size if compressed else 24 + size * 3
        if start + length > len(stream):
            break
        try:
            raw = zlib.decompress(stream[start + 16:start + length]) if compressed else stream[start:start + length]
            command, channel, count, sec, us = struct.unpack_from('<HHIQQ', raw)
            assert command == 3 and count == width * height and len(raw) == 24 + count * 3 and us < 1_000_000
        except (zlib.error, AssertionError, struct.error):
            assert not incoming, 'Invalid frame after stream synchronization'
            start = stream.find(b'EVAD', start + 1)
            continue
        end = start + length
        incoming.append({'due_ns': sec * 1_000_000_000 + us * 1000,
                         'arrival_ns': arrival_times[bisect.bisect_left(ends, end)], 'hash': identity(raw[24:])})
        start = end
    assert incoming, 'No valid incoming frames found'
    lookup = collections.defaultdict(list)
    for index, frame in enumerate(incoming):
        lookup[frame['hash']].append(index)
    rows, coverage = {}, set()
    refreshes = []
    partial = 0
    duplicate_pixels = 0
    syncs_without_pixels = 0
    for time_ns, b in packets(output_path):
        if b[:6] != bytes.fromhex('112233445566'):
            continue
        if b[12] == 0x55:
            y, x, count = struct.unpack_from('>HHH', b, 13)
            assert y < height and x + count <= width and len(b) >= 21 + count * 3
            row = rows.setdefault(y, bytearray(width * 3))
            row[x * 3:(x + count) * 3] = b[21:21 + count * 3]
            for column in range(x, x + count):
                key = (y, column)
                duplicate_pixels += key in coverage
                coverage.add(key)
        elif b[12] == 1:
            if not rows:
                syncs_without_pixels += 1
                continue
            if len(coverage) != width * height:
                partial += 1
                rows, coverage = {}, set()
                continue
            planes = []
            wire_order = output.get('color_order', 'RGB')
            for channel in 'RGB':
                channel_rows = [rows[y][wire_order.index(channel)::3] for y in range(height)]
                if output.get('flip_y', False):
                    channel_rows.reverse()
                if output.get('flip_x', False):
                    channel_rows = [row[::-1] for row in channel_rows]
                planes.append(b''.join(channel_rows))
            frame_hash = hashlib.sha256(b''.join(planes)).hexdigest()
            candidates = [i for i in lookup.get(frame_hash, []) if abs(incoming[i]['due_ns'] - time_ns) < 100_000_000]
            source_index = min(candidates, key=lambda i: abs(incoming[i]['due_ns'] - time_ns)) if candidates else None
            refreshes.append({'sync_ns': time_ns, 'hash': frame_hash, 'source_index': source_index})
            rows, coverage = {}, set()
    assert refreshes, 'No complete output frames found'
    changes = [r for i, r in enumerate(refreshes) if i == 0 or r['hash'] != refreshes[i - 1]['hash']]
    holds = []
    for i, r in enumerate(refreshes):
        if i == 0 or r['hash'] != refreshes[i - 1]['hash']:
            holds.append(1)
        else:
            holds[-1] += 1
    matched = [r for r in refreshes if r['source_index'] is not None]
    steps = [b['source_index'] - a['source_index'] for a, b in zip(matched, matched[1:])]
    incoming_changes = [f for i, f in enumerate(incoming) if i == 0 or f['hash'] != incoming[i - 1]['hash']]
    summary = {
        'incoming_frames': len(incoming), 'outgoing_complete_refreshes': len(refreshes),
        'incoming_timestamp_cadence': intervals([f['due_ns'] for f in incoming]),
        'incoming_arrival_cadence': intervals([f['arrival_ns'] for f in incoming]),
        'incoming_content_change_cadence': intervals([f['due_ns'] for f in incoming_changes]),
        'outgoing_refresh_cadence': intervals([r['sync_ns'] for r in refreshes]),
        # The first captured image may already be halfway through its hold.
        'outgoing_new_content_cadence': intervals([r['sync_ns'] for r in changes[1:]]),
        'outgoing_content_changes_including_initial': len(changes),
        'outgoing_observed_content_transitions': len(changes) - 1,
        'unchanged_refreshes': len(refreshes) - len(changes),
        'content_hold_refresh_counts_excluding_capture_edges': dict(collections.Counter(holds[1:-1])),
        'matched_refreshes': len(matched), 'source_index_step_counts': dict(collections.Counter(steps)),
        'pixel_duplicates': duplicate_pixels, 'partial_frames_at_sync': partial,
        'syncs_without_pixels': syncs_without_pixels,
        'queue_depth_min_max': [min(s[0] for s in statuses), max(s[0] for s in statuses)] if statuses else None,
        'method': 'SHA-256 of every reconstructed RGB pixel, undoing configured color order and flips; presentation timestamp is the outgoing sync capture time. Measures host transmission, not optical panel scanout.'}
    pathlib.Path(str(prefix) + '.json').write_text(json.dumps(summary, indent=2) + '\n')
    with open(str(prefix) + '.csv', 'w') as f:
        writer = csv.writer(f)
        writer.writerow(['refresh', 'sync_epoch_ns', 'interval_ms', 'content_changed', 'source_index', 'source_due_epoch_ns', 'rgb_sha256'])
        for i, r in enumerate(refreshes):
            index = r['source_index']
            writer.writerow([i, r['sync_ns'], (r['sync_ns'] - refreshes[i - 1]['sync_ns']) / 1e6 if i else '',
                             i == 0 or r['hash'] != refreshes[i - 1]['hash'], index,
                             incoming[index]['due_ns'] if index is not None else '', r['hash']])
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    if len(sys.argv) not in (5, 6):
        raise SystemExit('Usage: colorlight_capture_audit.py CONFIG INPUT.pcap OUTPUT.pcap REPORT_PREFIX [PORT]')
    input_port = int(sys.argv[5]) if len(sys.argv) == 6 else 49153
    if not 1 <= input_port <= 65535:
        raise SystemExit('PORT must be between 1 and 65535')
    audit(json.loads(pathlib.Path(sys.argv[1]).read_text()), *sys.argv[2:5], input_port=input_port)
