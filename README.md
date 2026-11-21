# NightDriver-Pi

NightDriver-Pi receives timestamped RGB frames from NDSCPP over TCP, buffers them, and presents them when due. Choose either the existing Raspberry Pi HUB75 adapter or optional raw Ethernet output to commissioned, stock-firmware Colorlight 5A-75B receiver cards. NDSCPP and the NightDriver wire protocol remain unchanged.

## Linux service

The optional `ndpi.service` unit starts playback at boot and can restore an audited
volatile receiver profile before playback. The startup helper and pixel sender use
the same JSON configuration, so both use the configured Colorlight interface.

See [deployment and recovery](docs/linux-service.md) for installation, interface isolation,
and receiver recovery.

## macOS launcher for the 512×32 display

With Xcode Command Line Tools installed, build and run:

```shell
make
./run.sh
```

The Mac build defaults to Ethernet output support without the Raspberry Pi matrix library. The root launcher reapplies this receiver's prepared RAM configuration for eight 64×32 panels, then starts playback over the switched LAN on `en35` at 512×32, using TCP port 49153 for NDSCPP input. It works from any working directory and forwards additional options to NDPi. Solid blue RGB `(0, 0, 128)` was visually confirmed across all eight panels. The first due NDSCPP frame replaces blue. Stop with Ctrl-C.

The launcher requires a locally generated bundle; set `NDPI_RAM_BUNDLE` if it is not at the default path in the ignored `receiver-backups/` directory. It reapplies the temporary configuration after a receiver power cycle; it does not save flash or EEPROM. See [the commissioning notes](docs/colorlight.md#temporary-receiver-configuration-during-development) before using it on another installation.

The complete FM6124 RAM profile and bounded macOS display-thread scheduling eliminate the observed refresh flicker. Duplicated pixel packets caused repeated image sections and an edge seam on this receiver. Sending each pixel packet once restored eight distinct static color bands with no seam. A single synchronization packet per refresh also removed the observed backward motion jumps. These settings are saved in the Mac example configuration. The current timing update uploads pixels 2 ms early and sends only sync at each precise 60 Hz deadline. It reduced measured sync jitter substantially, and the user reports stalls improved or gone in both the diagnostic pattern and normal live playback.

`./run.sh` uses the verified switched LAN path, equivalent to `./tools/run-colorlight-mac.sh --lan`. For a direct receiver connection on `en0`, use `./tools/run-colorlight-mac.sh` instead. Keep the raw Colorlight traffic on a dedicated receiver interface or VLAN; verify that the switches pass those frames within that segment. LAN mode reapplies the complete FM6124 RAM profile on startup, including after a receiver power cycle. Discovery replies remain unavailable over this path. See [LAN setup](docs/colorlight.md#this-mac-over-the-switched-lan).

To use the Mac launcher, first stop any other sender, including `ndpi.service`, so only one host sends to the receiver. For the built-in counter/motion test on the Mac, stop Mac playback and run `./run.sh --test-pattern`. It generates 30 fps content at 60 Hz, repeating each image twice. Use `--test-pattern --test-fps=60` to generate a new image every refresh. Colorlight test mode draws in native receiver coordinates, bypassing input flips, and runs locally without opening the NightDriver input socket. See the [test-pattern guide](docs/colorlight.md#built-in-counter-and-motion-pattern).

This development Mac already grants the current user access to `/dev/bpf*`, so `sudo` is unnecessary here. Port 49153 avoids the `rapportd` service currently listening on 49152; use the same port in NDSCPP later. The receiver's saved panel/layout configuration and the configured firmware profile must match the hardware. See [macOS setup](docs/colorlight.md#macos) for details.

## Building

Use a C++20 compiler, Make, and zlib development headers. On Raspberry Pi OS/Debian:

```shell
sudo apt install build-essential zlib1g-dev
```

For the default Raspberry Pi/Linux build, initialize the [`rpi-rgb-led-matrix`](https://github.com/hzeller/rpi-rgb-led-matrix) submodule:

```shell
git clone --recurse-submodules https://github.com/davepl/NightDriver-Pi.git
cd NightDriver-Pi
make
```

For an existing checkout, run `git submodule update --init --recursive` before `make`. The default Linux build includes both outputs and builds the matrix library as needed. macOS defaults to `WITH_HUB75=0`.

A Colorlight-only build skips the matrix library and needs no GPIO support or submodule:

```shell
make WITH_HUB75=0
```

Raw Ethernet output runs natively on Linux through `AF_PACKET` and on macOS through BPF. The portable tests can run without receivers.

## Running

Existing HUB75 invocations keep working; HUB75 remains the default, with the existing matrix flags, horizontal mirror, luminance correction, and VSync behavior:

```shell
sudo ./ndpi --led-rows=32 --led-cols=64 --led-chain=8
```

For Colorlight, commission the cards separately, update [the example wall configuration](examples/colorlight-wall.json), and select the output:

```shell
sudo ./ndpi --output=colorlight --config=examples/colorlight-wall.json
```

Use `./ndpi --help` for options. Colorlight mode uses the configured dimensions, opens the Ethernet transport, and never initializes GPIO. See [the Colorlight guide](docs/colorlight.md) for firmware profiles, geometry limits, permissions, dedicated Gigabit Ethernet setup, NDSCPP configuration, packet capture, and commissioning versus playback.

## Tests

```shell
make test
```

The focused tests use fake output/transport where needed; no GPIO, receiver card, or raw-socket privileges are required. They cover configuration validation, Ethernet packet bytes and splitting, color and orientation, input frame-size validation, and output selection. Hardware validation still requires the color/grid, receiver-boundary, NDSCPP streaming, timing, and sustained-operation checks listed in [the guide](docs/colorlight.md).

Tests also require Python 3. The guide describes an optional Linux integration test on an isolated virtual Ethernet network and a macOS startup test on the receiver-facing interface.
