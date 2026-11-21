"""Optional Linux smoke test. Pass ndpi and an isolated virtual Ethernet interface.

This emits raw Colorlight traffic; use a disposable internal Docker network or
veth pair, without physical receiver/network connections. Requires CAP_NET_RAW.
The portable `make test` suite does not invoke this test.
"""
import json
import pathlib
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

binary = str(pathlib.Path(sys.argv[1]).resolve())
interface = sys.argv[2]
addresses = bytes.fromhex("112233445566222233445566")
colors = bytes([10, 20, 30, 40, 50, 60])

with tempfile.TemporaryDirectory() as directory:
    config = pathlib.Path(directory) / "wall.json"
    config.write_text(json.dumps({
        "width": 2, "height": 1,
        "output": {"type": "colorlight", "interface": interface,
                   "firmware_profile": "legacy", "refresh_hz": 30,
                   "receivers": [{"index": 0, "x": 0, "y": 0, "width": 2, "height": 1}]}}))
    with socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3)) as capture:
        capture.bind((interface, 0))
        capture.settimeout(3)
        process = subprocess.Popen([binary, f"--config={config}"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 3
            while True:
                try:
                    client = socket.create_connection(("127.0.0.1", 49152), timeout=1)
                    break
                except OSError:
                    assert time.monotonic() < deadline, "ndpi did not start listening"
                    assert process.poll() is None, process.communicate()
                    time.sleep(0.02)
            with client:
                due = time.time() + 0.15
                seconds = int(due)
                micros = int((due - seconds) * 1_000_000)
                frame = struct.pack("<HHIQQ", 3, 0, 2, seconds, micros) + colors
                compressed = zlib.compress(frame)
                client.sendall(struct.pack("<IIII", 0x44415645, len(compressed), len(frame), 0) + compressed)
                response = b""
                while len(response) < 64:
                    part = client.recv(64 - len(response))
                    assert part, "missing NightDriver status"
                    response += part
                status = struct.unpack("<II5d4I", response)
                assert status[0] == 64 and status[7] == 500 and status[8] == 1

                # See the scheduled pixels followed by display sync, then the
                # same full frame and sync again without another input packet.
                presentations = 0
                rows_ready = False
                while presentations < 2:
                    packet = capture.recv(2048)
                    if packet[:12] != addresses:
                        continue
                    if packet[12] == 0x55 and packet[21:27] == colors:
                        assert time.time() >= due - 0.005, "future frame was presented early"
                        assert packet[13:21] == bytes.fromhex("0000000000020888")
                        rows_ready = True
                    elif packet[12] == 1 and rows_ready:
                        presentations += 1
                        rows_ready = False
                process.send_signal(signal.SIGTERM)
                stdout, stderr = process.communicate(timeout=2)
                assert process.returncode == 0, (stdout, stderr)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()
print("Linux raw Ethernet, compressed TCP, timestamps, keepalive and shutdown passed")
