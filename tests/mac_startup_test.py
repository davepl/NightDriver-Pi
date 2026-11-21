"""Optional macOS hardware test: emit/capture startup blue, then stop ndpi.

Usage: python3 tests/mac_startup_test.py ./ndpi CONFIG [PORT] [FRAMES]
Requires BPF access. This sends pixels to the configured physical interface.
No NDSCPP input is generated. Ordinary `make test` never invokes this script.
"""
import json
import pathlib
import signal
import struct
import subprocess
import sys
import tempfile
import time

binary = str(pathlib.Path(sys.argv[1]).resolve())
config_path = pathlib.Path(sys.argv[2]).resolve()
config = json.loads(config_path.read_text())
port = sys.argv[3] if len(sys.argv) > 3 else "49153"
interface = config["output"]["interface"]
width, height = config["width"], config["height"]
copies = 2 if config["output"]["firmware_profile"] == "v13" else 1
sync_copies = config["output"].get("sync_packet_count", copies)
pixel_copies = 2 if config["output"].get("duplicate_pixel_packets", False) else 1
target_frames = int(sys.argv[4]) if len(sys.argv) > 4 else 2
assert 2 <= target_frames <= 600
packet_pixels = config["output"].get("max_pixels_per_packet", 497)
packets_per_frame = pixel_copies * height * ((width + packet_pixels - 1) // packet_pixels) + copies + sync_copies

with tempfile.TemporaryDirectory() as directory:
    pcap = pathlib.Path(directory) / "blue.pcap"
    capture = subprocess.Popen([
        "tcpdump", "-i", interface, "-n", "-s", "0", "-U", "-c", str(packets_per_frame * target_frames),
        "-w", str(pcap), "ether dst 11:22:33:44:55:66"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    process = None
    try:
        time.sleep(0.3)
        assert capture.poll() is None, capture.communicate()
        process = subprocess.Popen([binary, "--output=colorlight", f"--config={config_path}", f"--port={port}"],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        _, capture_errors = capture.communicate(timeout=3 + 2 * target_frames / config["output"].get("refresh_hz", 30))
        assert capture.returncode == 0, capture_errors.decode()
        assert process.poll() is None, process.communicate()
    finally:
        if capture.poll() is None:
            capture.kill()
            capture.communicate()
        if process is not None and process.poll() is None:
            process.send_signal(signal.SIGTERM)
            try:
                stdout, stderr = process.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                stdout, stderr = process.communicate()
                raise AssertionError(f"ndpi did not stop after SIGTERM: {stdout!r} {stderr!r}")
            assert process.returncode == 0, (stdout, stderr)

    data = pcap.read_bytes()
    endian = {b"\xd4\xc3\xb2\xa1": "<", b"\xa1\xb2\xc3\xd4": ">",
              b"\x4d\x3c\xb2\xa1": "<", b"\xa1\xb2\x3c\x4d": ">"}[data[:4]]
    assert struct.unpack_from(endian + "I", data, 20)[0] == 1, "capture must use Ethernet"
    offset = 24
    covered = {}
    sync_times = []
    timestamp_scale = 1e9 if data[:4] in (b"\x4d\x3c\xb2\xa1", b"\xa1\xb2\x3c\x4d") else 1e6
    frames = 0
    while offset < len(data):
        seconds, fraction, length, _ = struct.unpack_from(endian + "4I", data, offset)
        packet = data[offset + 16:offset + 16 + length]
        offset += 16 + length
        assert packet[:12] == bytes.fromhex("112233445566222233445566"), "Ethernet MAC header changed"
        if packet[12] == 0x55:
            y, x, count = struct.unpack_from(">HHH", packet, 13)
            assert y < height and x + count <= width and 0 < count <= packet_pixels
            expected_blue = bytes((128, 0, 0) if config["output"].get("color_order", "RGB") == "BGR" else (0, 0, 128))
            assert packet[21:21 + count * 3] == expected_blue * count, "startup pixel is not blue"
            for column in range(x, x + count):
                key = (column, y)
                covered[key] = covered.get(key, 0) + 1
                assert covered[key] <= pixel_copies, "unexpected duplicate pixel before display sync"
        elif packet[12] == 1 and covered:
            assert len(covered) == width * height, "incomplete frame before display sync"
            assert all(count == pixel_copies for count in covered.values()), "missing pixel duplicate"
            sync_times.append(seconds + fraction / timestamp_scale)
            frames += 1
            covered.clear()
    assert frames == target_frames and not covered, "startup and idle refresh must all be complete"
    if target_frames >= 60:
        import statistics
        intervals = [b - a for a, b in zip(sync_times[2:], sync_times[3:])]
        expected = 1 / config["output"].get("refresh_hz", 30)
        median = statistics.median(intervals)
        assert abs(median - expected) < expected * 0.02, (median, expected)
        assert max(abs(gap - expected) for gap in intervals) < 0.002, (min(intervals), max(intervals))
        print(f"Frame intervals: min={min(intervals)*1000:.3f}, median={median*1000:.3f}, max={max(intervals)*1000:.3f} ms")
print(f"Captured {target_frames} complete {width}x{height} blue frames on {interface}; startup, idle refresh and shutdown passed")
