# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline CLI regression: link a throwing transport double; hardware is absent."""
import json
import os
import pathlib
import struct
import subprocess
import tempfile
import zlib

root = pathlib.Path(__file__).resolve().parents[1]
addresses = bytes.fromhex("112233445566222233445566")


def packet(kind, size):
    frame = bytearray(size)
    frame[:12] = addresses
    frame[12] = kind
    return frame


with tempfile.TemporaryDirectory(prefix="ndpi-ram-cli-test-") as folder:
    folder = pathlib.Path(folder)
    stub = folder / "transport.cpp"
    stub.write_text('#include "colorlight.h"\n#include <stdexcept>\n'
                    'std::unique_ptr<EthernetTransport> CreateColorlightTransport(const std::string &name) '
                    '{ throw std::runtime_error("TEST_TRANSPORT_OPENED " + name); }\n')
    binary = folder / "ram-config"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", f"-I{root}",
                    str(root / "tools/colorlight_ram_config.cpp"), str(root / "outputconfig.cpp"), str(stub), "-o", str(binary)], check=True)

    # Fresh synthetic basic body and headers; no vendor packet payloads.
    image = bytearray(65536)
    for offset, value in {0: 0xA8, 4: 16, 5: 64, 6: 8, 7: 16, 8: 14, 0x10: 0x24,
                          0x88: 2, 0x8B: 32}.items():
        image[offset] = value
    image[252:256] = struct.pack("<I", zlib.crc32(image[:252]))
    image_path = folder / "image.bin"
    image_path.write_bytes(image)
    basic = packet(5, 272)
    basic[16:] = image[:256]
    area = packet(2, 1296)
    module = packet(0x17, 785)
    module[15:17] = b"\xff\xff"
    frames = [basic, area, module]
    bundle = b"NDRAM01\0" + struct.pack("<H", len(frames))
    bundle += b"".join(struct.pack("<H", len(frame)) + frame for frame in frames)
    bundle_path = folder / "packets.bin"
    bundle_path.write_bytes(bundle)
    forbidden = bytearray(bundle)
    forbidden[24] = 0x19
    forbidden_path = folder / "forbidden.bin"
    forbidden_path.write_bytes(forbidden)

    def run(arguments, code, expected, opened=False):
        result = subprocess.run([str(binary), *map(str, arguments)], capture_output=True, text=True, timeout=3)
        output = result.stdout + result.stderr
        assert result.returncode == code and expected in output, output
        assert ("TEST_TRANSPORT_OPENED" in output) == opened, output

    run(["--image", image_path], 0, "runtime_routing=incomplete")
    run(["--image", image_path, "--interface", "en0", "--apply-ram"], 1, "image lacks runtime routing tables")
    run(["--packets", bundle_path], 0, "source=audited-packets")
    run(["--packets", bundle_path, "--interface", "en0"], 0, "mode=dry-run")
    run(["--packets", bundle_path, "--packet-gap-ms=100"], 0, "gap_ms=100")
    run(["--packets", bundle_path, "--packet-gap-ms", "1000"], 0, "gap_ms=1000")
    for gap in ("0", "1001", "-1", "1x", "", "99999999999999999999"):
        run(["--packets", bundle_path, f"--packet-gap-ms={gap}", "--interface", "en0", "--apply-ram"],
            1, "must be an integer from 1 to 1000")
    run(["--packets", bundle_path, "--packet-gap-ms=100", "--packet-gap-ms=100"],
        1, "Duplicate --packet-gap-ms")
    run(["--packets", forbidden_path, "--interface", "en0", "--apply-ram"], 1, "Unapproved packet type")
    run(["--packets", bundle_path, "--image", image_path], 1, "mutually exclusive")
    config_path = folder / "colorlight config.json"
    config = json.loads((root / "examples/colorlight-wall.json").read_text())
    config["output"]["interface"] = "lighting.3"
    config_path.write_text(json.dumps(config))
    run(["--packets", bundle_path, "--config", config_path], 0, "mode=dry-run")
    run(["--packets", bundle_path, "--config", config_path, "--apply-ram"],
        1, "TEST_TRANSPORT_OPENED lighting.3", True)
    run(["--packets", bundle_path, f"--config={config_path}", "--apply-ram"],
        1, "TEST_TRANSPORT_OPENED lighting.3", True)
    run(["--packets", bundle_path, "--config", config_path, "--interface", "eth0", "--apply-ram"],
        1, "--config and --interface are mutually exclusive")
    run(["--packets", bundle_path, "--config", config_path, "--config", config_path, "--apply-ram"],
        1, "Duplicate --config")
    run(["--packets", bundle_path, "--config=", "--apply-ram"], 1, "--config requires a nonempty path")
    run(["--packets", bundle_path, "--config", folder / "missing.json", "--apply-ram"], 1, "cannot open")
    del config["output"]["interface"]
    config_path.write_text(json.dumps(config))
    run(["--packets", bundle_path, "--config", config_path, "--apply-ram"], 1, "interface")
    run(["--packets", bundle_path, "--apply-ram"], 1, "--apply-ram requires --config")
    # These requests can reach only the throwing double, never a network device.
    run(["--packets", bundle_path, "--interface", "en0", "--apply-ram"], 1, "TEST_TRANSPORT_OPENED", True)
    run(["--image", image_path, "--area-only", "--interface", "en0", "--apply-ram"],
        1, "TEST_TRANSPORT_OPENED", True)

print("RAM CLI source-selection, image-apply refusal and no-open dry-run tests passed")
