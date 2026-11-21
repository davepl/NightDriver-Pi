"""Check the capture auditor against deliberate repeats and a backward frame."""
import contextlib
import importlib.util
import io
import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib

sys.dont_write_bytecode = True
audit_path = pathlib.Path(__file__).resolve().parents[1] / 'tools/colorlight_capture_audit.py'
spec = importlib.util.spec_from_file_location('capture_audit', audit_path)
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


def pcap(path, packets):
    with open(path, 'wb') as f:
        f.write(struct.pack('<IHHIIII', 0xa1b2c3d4, 2, 4, 0, 0, 65535, 1))
        for micros, packet in packets:
            f.write(struct.pack('<IIII', micros // 1_000_000, micros % 1_000_000, len(packet), len(packet)))
            f.write(packet)


def tcp_packet(source_port, destination_port, seq, payload):
    packet = bytearray(54)
    packet[12:14] = b'\x08\x00'
    packet[14] = 0x45
    packet[23] = 6
    struct.pack_into('>H', packet, 16, 40 + len(payload))
    struct.pack_into('>HHI', packet, 34, source_port, destination_port, seq)
    packet[46] = 0x50
    packet.extend(payload)
    return packet


def frame_payload(rgb, index):
    raw = struct.pack('<HHIQQ', 3, 0, 2, 1000, index * 33333) + rgb
    compressed = zlib.compress(raw)
    return struct.pack('<4I', 0x44415645, len(compressed), len(raw), 0) + compressed


with tempfile.TemporaryDirectory() as directory:
    root = pathlib.Path(directory)
    frames = [bytes((i + 1, 10, 20, i + 30, 40, 50)) for i in range(3)]
    output = []
    for tick, index in enumerate([0, 0, 0, 1, 2, 1, 1]):
        packet = bytearray(60)
        packet[:6] = bytes.fromhex('112233445566')
        packet[12] = 0x55
        struct.pack_into('>HHH', packet, 13, 0, 0, 2)
        # Undo the configured flip and BGR conversion in the generated sample.
        rgb = frames[index]
        packet[21:27] = rgb[3:6][::-1] + rgb[0:3][::-1]
        now = 1_000_000_000 + tick * 16666
        output.append((now - 100, packet))
        sync = bytearray(112)
        sync[:6] = packet[:6]
        sync[12] = 1
        output.append((now, sync))
    pcap(root / 'output.pcap', output)
    config = {'width': 2, 'height': 1, 'output': {'color_order': 'BGR', 'flip_x': True}}
    (root / 'config.json').write_text(json.dumps(config))
    for input_port in (49153, 49152):
        source = []
        seq = 1234
        for index, rgb in enumerate(frames):
            payload = frame_payload(rgb, index)
            source.append((998_000_000 + index * 33333, tcp_packet(12345, input_port, seq, payload)))
            seq += len(payload)
        # A valid stream on another port must not enter the frame reconstruction.
        other_port = 49152 if input_port == 49153 else 49153
        source.append((998_100_000, tcp_packet(12346, other_port, 900000, frame_payload(bytes(6), 0))))
        for port, depth in ((input_port, 5), (input_port, 7), (other_port, 999)):
            status = struct.pack('<II5d4I', 64, 0, 0, 0, 0, 0, 0, 500, depth, 0, 0)
            source.append((998_200_000 + depth, tcp_packet(port, 12345, 500000, status)))
        pcap(root / 'input.pcap', source)
        if input_port == 49153:
            # Omitting the port keeps the original API default working.
            with contextlib.redirect_stdout(io.StringIO()):
                audit.audit(config, root / 'input.pcap', root / 'output.pcap', root / 'result')
        else:
            # Exercise the optional CLI argument as used by deployed captures.
            subprocess.run([sys.executable, str(audit_path), str(root / 'config.json'),
                            str(root / 'input.pcap'), str(root / 'output.pcap'),
                            str(root / 'result'), str(input_port)], check=True, capture_output=True, text=True)
        result = json.loads((root / 'result.json').read_text())
        assert result['incoming_frames'] == 3
        assert result['queue_depth_min_max'] == [5, 7]
        assert result['outgoing_complete_refreshes'] == 7
        assert result['outgoing_observed_content_transitions'] == 3
        assert result['unchanged_refreshes'] == 3
        assert result['source_index_step_counts'] == {'0': 3, '1': 2, '-1': 1}
        assert result['matched_refreshes'] == 7
        assert result['partial_frames_at_sync'] == result['pixel_duplicates'] == 0
print('capture identity, port filtering, repeat detection, orientation and backward-step tests passed')
