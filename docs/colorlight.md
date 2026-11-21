# Colorlight 5A-75B output

NightDriver-Pi can receive one complete display rectangle from NDSCPP and send it over Ethernet to already commissioned, stock-firmware Colorlight 5A-75B receivers:

```text
NDSCPP LEDFeature → TCP/49152 (or --port) → ndpi → dedicated Gigabit Ethernet → receiver chain
```

The Pi remains an ordinary NightDriver endpoint. Compression, timestamp scheduling, buffering, channel masks, and status responses use the same input path as HUB75. Colorlight output creates no `RGBMatrix` and initializes no GPIO.

## Commission the receivers first

Use LEDVISION or another verified commissioning tool to configure each card's panel driver, scan mode, cabling, dimensions, and region within the overall display. Save and check that configuration with stock receiver firmware before switching to the Pi.

**The JSON `receivers` entries describe the expected saved layout. They do not discover cards or write any receiver configuration.** The first implementation does not program panel settings, card addresses, or layout. Setting a rectangle in JSON cannot move a card's displayed region.

All cards see a shared pixel coordinate space. A receiver's commissioned configuration determines which region it displays. Receiver `index` values identify the expected cards for documentation and validation; they are neither destination IP addresses nor NightDriver channels. Use one logical input stream for the entire rectangle, regardless of how many cards display it.

## Network and permissions

Use a dedicated Ethernet interface on Linux or macOS with a working **1 Gbit/s full-duplex** connection to the receiver chain. Connect directly or through equipment already known to pass Colorlight's Ethernet traffic. Keep management/NDSCPP traffic on another interface, such as the Pi's Wi-Fi or a second Ethernet adapter. A card has no per-pixel destination IP address, and this output needs no IP address, subnet, gateway, UDP port, or TCP port on the receiver-facing interface.

These are raw Ethernet frames, not IP traffic. Ordinary routing, Wi-Fi links, and IP tunnels cannot carry them directly. Confirm the exact card revision and firmware support the selected profile; a different Colorlight model is not automatically compatible.

### macOS

The native macOS backend writes complete Ethernet frames through `/dev/bpf*`, preserving Colorlight's protocol MAC addresses. No Linux VM, Docker network, or IP configuration is needed for playback.

For the current 512×32 display connected to the development Mac:

```shell
make
ifconfig en0
./tools/run-colorlight-mac.sh
```

[The Mac configuration](../examples/colorlight-mac-512x32.json) describes the expected receiver region at `(0, 0)` with size 512×32. It uses BGR wire order, horizontal flip, brightness 255, 497-pixel fragments, single-copy pixel packets, one synchronization packet per refresh, and 60 Hz playback. Startup blue itself is RGB `(0, 0, 128)`. `en0` is the current receiver-facing interface, with an active `1000baseT` full-duplex link. The Mac's ordinary network uses `en35`. These interface assignments are specific to this host; inspect `networksetup -listallhardwareports` and `ifconfig` on another Mac.

The backend requires read/write access to a free BPF device. The current user belongs to `access_bpf`, and the devices already grant that group access, so the command above runs without `sudo`. On a Mac without BPF access, run the command with `sudo`; the program reports a permissions error when access is unavailable.

The macOS `rapportd` service currently occupies TCP port 49152 on this host. `--port=49153` chooses an available NightDriver input port without changing that service. The usual default remains 49152. When connecting NDSCPP, use the Mac's reachable address, port 49153, width 512, and height 32 in its ordinary LEDFeature configuration.

Colorlight sends solid blue RGB `(0, 0, 128)` at startup and periodically refreshes it while waiting for input. The first due NDSCPP frame replaces that startup image; a later input pause retains the last displayed frame. Ctrl-C or SIGTERM stops transmission and closes BPF and input sockets. Whether the receiver holds or blanks after shutdown depends on its saved settings. Discovery on the current card reported receiver 0 with firmware 13.17, confirming the example's `v13` profile.

The physical display has eight P3-6432-2121-16S-D1.0 modules, each 64×32 with FM6124 drivers, chained from the first HUB75 output for a 512×32 display. The local launcher was tested end to end, and blue coverage was visually confirmed across all eight panels. The complete FM6124 profile and bounded macOS display-thread scheduling produced steady blue with no visible refresh flicker. Duplicated pixel packets caused repeated sections with 128-pixel fragments and an edge seam with 497-pixel fragments. Sending each pixel packet once at 497+15 pixels per row restored eight distinct static color bands with no seam. A single synchronization packet per refresh then eliminated the observed backward jumps in live motion.

#### Temporary receiver configuration during development

The working Pi library is the reference for the panel's electrical behavior. Its default `panel_type` is null, so `Framebuffer::InitializePanels` skips special chip-register initialization. The ordinary HUB75 path uses binary ABCD addresses for 16 scan positions, paired upper/lower RGB data, and 512 clocks followed by one latch pulse for this eight-panel chain. Those facts establish the geometry and identity mapping. Colorlight's grayscale depth, segment schedule, clock code, and OE timing remain receiver-specific; they cannot be copied directly from the Pi's option numbers. A generic factory-timing profile flickered badly. The complete vendor FM6124 preset, regenerated for eight panels, substantially reduced flicker; bounded macOS scheduling then eliminated the remaining visible refresh flicker.

`make probe ram-config` builds two separate development utilities. `build/tools/colorlight-probe --interface en0` performs read-only discovery; its optional page reads access stored flash, not the complete active RAM state. `build/tools/colorlight-ram-config --packets BUNDLE` validates a local bundle offline. Adding `--config wall.json --apply-ram` (or `--interface en0 --apply-ram` for a manual operation) transmits its reviewed volatile parameter frames without saving flash or EEPROM. Full application from a compiled image is refused because the image alone lacks runtime routing tables.

The locally generated profiles and original receiver backup are kept in the ignored `receiver-backups/` directory. Vendor-generated parameter payloads are not distributed with the source. The current FM6124 profile bundle is `fm6124-512x32-runtime-complete.ndram`; it contains all 51 SDK-generated runtime frames, including routing, gamma, two scan-table packets, and 22 runtime shadow-record packets. A power cycle clears this temporary receiver configuration.

The initial loader incorrectly grouped the shadow-record packets with persistent EEPROM writes. The SDK distinguishes opcode-zero type `0x1B`/`0x2B` runtime updates from its type `0x19`/opcode `0x85` save path. The validator now permits only the reviewed runtime addresses, lengths, opcode zero, and zero padding; persistent write/save opcodes remain rejected. The stored flash-mirror page was byte-identical before and after application. This corrects a configuration omission, but the complete generic profile still has continuous-refresh flicker on the current hardware.

These RAM frames have no verified acknowledgement or complete active-state readback. `mode=sent-ram` means transmission completed, not that every table was applied. Physical blue output across all eight panels supplied the acceptance check for the current setup. Normal `ndpi` playback never invokes either commissioning utility.

Earlier debugging used 320×32 geometry after the chain was mistakenly reported as five panels. That profile produced green banks or a sharp brightness boundary, and smaller pixel packets did not resolve it. Once the physical count was corrected to eight, regenerating the complete 512×32 receiver profile and sending a matching 512×32 frame produced uniform blue. Chain length must match the physical shift chain; updating the JSON alone does not regenerate the receiver's routing and scan-timing tables.

The local launcher, `./tools/run-colorlight-mac.sh`, reapplies the FM6124 profile three times with 100 ms between packets and a one-second pause after each pass before starting normal blue playback. An earlier capture verified that delivery method byte for byte and measured at least 100 ms between packets. The launcher needs the ignored local bundle described above. `--packet-gap-ms` accepts 1–1000; the standalone RAM tool's default remains 5 ms. The current vendor preset uses grayscale depth 16, 40 segments, and clock setting 7. The stored flash-mirror page remained byte-identical after applying both complete runtime profiles.

For a receiver whose active configuration already matches the display, playback alone is `./ndpi --output=colorlight --config=examples/colorlight-mac-512x32.json --port=49153`.

### This Mac over the switched LAN

Run `./run.sh` for playback on the Mac's 25 GbE PCIe interface `en35`, using `examples/colorlight-mac-lan-512x32.json` and input TCP port 49153. The root launcher delegates to `./tools/run-colorlight-mac.sh --lan` and forwards options such as `--test-pattern --test-fps=60`. When calling the helper directly, `--lan` must precede any pattern flags. The direct launcher remains `./tools/run-colorlight-mac.sh` on `en0`.

Use a dedicated Layer-2 segment or VLAN for the receiver traffic. The sender-side VLAN interface should carry both the volatile startup profile and pixel frames. Carry the VLAN tag only on the necessary trunk links and present the receiver-facing Ethernet port as an untagged access port on that VLAN. IGMP snooping does not register Colorlight's non-IP destination MAC; verify forwarding only within the dedicated segment.

Discovery replies are not required for playback. A prior test reproduced a folded image after power cycling the receiver. The outgoing frame capture was complete, but only reapplying the full volatile profile restored correct output.

To capture this link using the current account's BPF access:

```shell
tcpdump -i en0 -e -s 0 -w colorlight.pcap 'ether dst 11:22:33:44:55:66'
```

A capture confirms transmitted bytes, not the visible panel result.

To check startup and idle refresh on this physical connection without sending NDSCPP input, run `python3 tests/mac_startup_test.py ./ndpi examples/colorlight-mac-512x32.json 49154 120`. This starts the sender, captures 120 complete blue frames, validates pixel bytes, duplicate counts, Ethernet headers and cadence, then checks clean shutdown. Port 49154 isolates startup blue from the live NDSCPP source on 49153. It requires BPF access and sends pixels to the configured receiver interface.

### Linux

On Raspberry Pi OS/Debian, install diagnostic/capability utilities with `sudo apt install ethtool tcpdump libcap2-bin` if needed. After replacing `eth0` with the dedicated interface name:

```shell
ip link show dev eth0
sudo ip link set dev eth0 up
ethtool eth0
```

Check that the interface is Ethernet, link detection succeeds, and `Speed: 1000Mb/s` / `Duplex: Full` are reported. Fix cabling, adapter, or negotiation problems before starting playback. Avoid assigning this interface a bridge or bond role while commissioning.

Opening the Linux `AF_PACKET` raw socket requires root or `CAP_NET_RAW`. The Colorlight-only executable can run without root after granting the capability:

```shell
sudo setcap cap_net_raw=ep ./ndpi
getcap ./ndpi
./ndpi --output=colorlight --config=examples/colorlight-wall.json
```

Rebuilding/replacing the binary can remove file capabilities; apply `setcap` to the actual installed executable again when needed. `sudo ./ndpi ...` is an alternative. HUB75 has its existing GPIO/library permission requirements.

## Local geometry and options

Start with [the complete example](../examples/colorlight-wall.json), change the interface, firmware profile, dimensions, and expected saved receiver rectangles, then run:

```shell
./ndpi --output=colorlight --config=wall.json
```

The top-level `width` and `height` define the complete row-major RGB rectangle received from NDSCPP. NightDriver's packet header contains a pixel count, not dimensions. Every input frame must contain exactly `width * height` pixels. Receiver `x`/`y` coordinates are local to this rectangle.

| Field | Meaning |
| --- | --- |
| `output.type` | Required: `"colorlight"`. |
| `output.interface` | Required: dedicated Ethernet interface name, such as `eth0` on Linux or `en0` on macOS. |
| `output.firmware_profile` | Required: `"legacy"` or `"v13"`; choose after checking the installed receiver firmware. The example uses `v13`, which is not a detection result. |
| `output.brightness` | Integer 0–255, default 255; receiver brightness control. |
| `output.color_order` | `"RGB"` by default; `"BGR"` swaps red/blue on the wire when required by the installed setup. |
| `output.flip_x`, `output.flip_y` | Boolean, both default `false`; transform the complete received rectangle before transmission. |
| `output.refresh_hz` | Integer 1–60, default 30; fixed playback cadence; at each tick, present the newest due input frame or retransmit the retained image. Missed ticks are skipped. |
| `output.max_pixels_per_packet` | Integer 1–497, default 497; current Mac uses 497. |
| `output.duplicate_pixel_packets` | Boolean, default false; duplicate each pixel packet consecutively. Current Mac uses false: duplicate pixel packets caused repeated sections and edge corruption. |
| `output.sync_packet_count` | Optional integer 1–2; overrides the profile’s display/sync count. Current Mac uses 1 to avoid backward motion jumps. |
| `output.prepare_ahead_us` | Integer 0–20000 and shorter than one refresh period; default 0. Positive values upload pixels before the intended display deadline, then send only synchronization at that deadline. The current Mac trial uses 2000. |
| `output.sync_gap_us` | Integer 0–20000, default 500; guard interval after pixel packets and before display synchronization. |
| `output.receivers` | Required nonempty list of expected card rectangles, each with `index`, `x`, `y`, `width`, `height`. |

This first implementation supports `width` and `height` from 1–65535, at most 655360 pixels in the complete received frame, and 1–64 receiver entries with unique indices from 0–63. Each receiver rectangle is limited to 8192 pixels per side and 65536 pixels in area. These are conservative software support limits: physical limits vary with card revision, firmware, panel driver, scan rate, and refresh rate. A configuration passing validation does not establish that its panel loading is supported by the hardware.

Unknown fields, invalid types, duplicate JSON keys or receiver indices, out-of-bounds rectangles, and overlapping rectangles are rejected. The configuration file is limited to 64 KiB. Gaps between receiver rectangles are allowed; the Pi still sends the complete logical display, including those gaps. Layout entries do not route or crop pixel packets.

Application options accept either `--option=value` or `--option value`. `--port` selects the incoming NightDriver TCP port from 1–65535 and defaults to 49152; NDSCPP must use the same port. With no application options, HUB75 remains the default. `--config=wall.json` also selects Colorlight when `--output` is omitted. An explicit `--output=hub75` conflicts with a Colorlight configuration and is rejected. Colorlight requires a configuration file. Matrix library flags remain available in HUB75 mode; they are rejected in Colorlight mode. Use `./ndpi --help` for application options and HUB75 flags available in the build.

HUB75 keeps its existing horizontal mirror, luminance correction, and framebuffer VSync swap. Colorlight defaults to unmirrored RGB with no added HUB75 luminance correction. Use the explicit Colorlight orientation/color fields to match your commissioned layout. Account for any `reversed` or `redGreenSwap` option already applied by NDSCPP when checking the resulting picture.

## NDSCPP configuration

Use [one normal LEDFeature](../examples/ndscpp-feature.json) for the complete display. This file is a feature object to insert into an existing NDSCPP canvas configuration, not a complete NDSCPP configuration file. Set `hostName` to the receiver host's reachable management address, use the same `port` selected by `ndpi` (normally 49152), and match `width`/`height` to the local configuration. For the current Mac setup, use port 49153 and size 512×32.

The feature's `offsetX` and `offsetY` locate the Pi's entire rectangle within NDSCPP's larger canvas. They do not replace or add to the local receiver rectangles in `wall.json`. For example, a 256×64 feature at canvas offset (512, 128) still uses receiver coordinates beginning at (0, 0) on the Pi.

Keep `channel` at 0 for the ordinary default endpoint. The existing receiver accepts channel 0 or masks containing bit 0; those semantics are unchanged. Do not assign a separate feature or channel to each Colorlight card. No NDSCPP change or NightDriver protocol extension is needed. The example uses `reversed: false` and `redGreenSwap: false` so the Pi receives the logical RGB image directly.

## Wire behavior and compatibility

The sender splits each row into packets of at most `max_pixels_per_packet` pixels (default 497), with explicit row, pixel offset, and pixel count fields. It sends the brightness control packet first, then all rows, then waits the configured guard interval before issuing the display/sync packet. The Ethernet destination is `11:22:33:44:55:66`, with protocol source `22:22:33:44:55:66`. These fixed addresses are protocol constants, not individual receiver identities.

The `legacy` profile sends each brightness and display/sync packet once; `v13` sends each twice to match FPP's handling of firmware 13 and later. The choice is explicit because older cards can be timing-sensitive and board revisions differ. `sync_packet_count` overrides only the display/sync count. The current firmware 13.17 receiver needs one sync for smooth motion; two caused backward jumps. Pixel packets are sent once unless `duplicate_pixel_packets` is enabled; duplicates caused repeated sections and an edge seam on this receiver. There is no automatic firmware discovery. Adjust the guard interval only after testing the actual receivers; a profile name is not a promise that every revision works.

Colorlight limits the queued RGB frame storage to approximately 256 MiB or 500 frames, whichever is smaller; status responses report the resulting capacity to NDSCPP. HUB75 retains its existing 500-frame capacity.

Geometry, row-packet overhead, network transmission time, and the synchronization guard interval limit the achievable frame rate. Keep the intended stream and refresh rate within the available link budget; accepted geometry alone does not guarantee 60 fps. Transmission takes time, so presentation is scheduled rather than hard real-time.

The last successfully presented frame is retained and periodically retransmitted as a conservative keepalive. Playback follows fixed deadlines at `refresh_hz`, independently of input arrival times and transmission duration. Each tick selects the newest due frame, dropping overdue intermediate frames instead of bursting multiple latches. HUB75 retains its existing VSync behavior. On macOS, only the display thread receives a bounded Mach time-constraint policy and uses absolute Mach waits; this does not synchronize to the receiver’s internal scan phase. A universal stock-firmware timeout has not been verified, and no-signal behavior depends on the commissioned card settings. The next incoming due frame replaces the retained image without introducing an independent input timeline. A failed send terminates playback with an error rather than issuing a display sync after an incomplete frame. Shutdown stops transmission and releases the socket; whether the card holds or blanks its last image is receiver behavior.

## Verification

Run the focused software tests using the commands in the [README](../README.md). They verify configuration rules, pixel packet bytes/splitting, color/orientation, frame-size rejection, and selecting Colorlight without GPIO initialization. Packet generation uses a fake transport and requires no receiver or raw-socket privilege. These tests cannot establish physical panel behavior.

An optional Linux integration test exercises the real raw socket, compressed TCP input, timestamp scheduling, idle retransmission, and shutdown: `python3 tests/linux_ethernet_test.py ./ndpi INTERFACE`. Run it with `CAP_NET_RAW` on an isolated virtual Ethernet interface, such as a disposable internal Docker network or veth pair, with no physical network or receivers attached. It binds TCP port 49152 and emits test packets. This virtual-network check also cannot establish physical panel behavior.

For receiver-side captures on the dedicated interface:

```shell
sudo tcpdump -i eth0 -e -s 0 -w colorlight.pcap 'ether dst 11:22:33:44:55:66'
```

Captures should show raw Colorlight packets with complete row sequences followed by control packets. A capture on the Pi proves what was submitted to the network stack, not what the panel displayed.

Before treating the installation as ready, check on the actual hardware:

1. Show solid red, green, and blue; verify channel order and brightness including 0 and 255.
2. Show distinct corner markers and a labelled coordinate/grid pattern; check origin, row direction, horizontal/vertical orientation, and full coverage.
3. Draw moving lines across every receiver boundary; check preconfigured card regions, gaps, and duplicated pixels.
4. Stream normal compressed and uncompressed frames from NDSCPP, including future timestamps; check synchronized motion and status responses.
5. Pause/reconnect NDSCPP and inspect retained-frame refresh. Check link interruption, error reporting, Ctrl-C/SIGTERM, and restarting playback.
6. Run a sustained stream at the intended frame rate and maximum intended geometry; watch for tearing, dropped rows, network errors, and thermal/resource problems.

Automated verification is separate from these hardware checks. Native macOS transmission was exercised on the directly connected `en0` interface; captures confirmed complete blue startup and idle packets, and shutdown passed. Correcting both receiver and sender geometry to 512×32 established blue coverage across all eight physical panels. The final single-copy, 497-pixel configuration passed a 120-frame hardware capture, including complete blue frames and shutdown, with a median interval of 16.666 ms (observed range 16.559–16.768 ms). The user confirmed eight distinct static color bands with no edge seam after disabling pixel duplication, and steady blue without visible refresh flicker. Live input capture showed 30 fps, each source frame advancing exactly one pixel; the outgoing 60 Hz stream repeated each source frame twice, with no out-of-order frames. The user confirmed smooth live motion after reducing synchronization to one packet per refresh. A later one-minute live capture found a constant 50 queued input frames (about 1.65 seconds ahead), no skipped or backward input steps, and exactly two refreshes per matched 30 fps source frame. The user still reports occasional physical stutter; source-matched 30 Hz output worsened flicker, so 60 Hz was restored. The remaining occasional stutter is unresolved. Full color/coordinate and sustained-operation validation remain unfinished.


### Measuring new images separately from refreshes

`tools/colorlight_capture_audit.py` reads simultaneous Ethernet pcaps of one compressed NDSCPP connection on TCP 49153 and the Colorlight output. Start the input capture at least two seconds earlier to include queued frames. Run:

```sh
python3 tools/colorlight_capture_audit.py CONFIG INPUT.pcap OUTPUT.pcap REPORT_PREFIX
```

It reconstructs complete frames, undoes configured color order and flips, and hashes every RGB pixel. Adjacent identical hashes count as retained-image refreshes; source-frame matching also detects skipped or backward frames. The JSON report gives arrival, source-timestamp, refresh, and new-content timing separately. The CSV records each sync timestamp, content-change flag, source index, and image hash. Capture timestamps measure host Ethernet transmission, not the receiver's optical scanout.

A fresh 3,600-refresh capture on this host matched every output image to its incoming source. All 1,799 fully observed interior image holds lasted exactly two refreshes. Source timestamps ran at 30.0000002 Hz; network frame arrivals averaged 30.0003222 Hz. Output refreshes ran at 60.0025682 Hz (16.271–17.049 ms), and actual image changes ran at 30.0012986 Hz (33.024–33.730 ms). No skipped/backward source frames, incomplete output frames, or pixel duplicates were observed; queue depth remained 50. First/last partial image holds are excluded from the hold distribution and the first partial interval is excluded from new-content cadence.


A later timing-margin comparison increased `sync_gap_us` from 500 to 2000 while retaining 60 Hz and single-copy pixels/sync. The actual captured last-pixel-to-sync gap increased from 0.568–0.847 ms to 2.086–2.313 ms; the user reported worse stalls. The canonical 500 µs setting was restored. This does not establish receiver-side latch deadlines or optical frame timing; the remaining missed-update hypothesis requires panel-side measurement.


### Built-in counter and motion pattern

Stop the running display process, then use the ordinary Mac launcher:

```sh
./tools/run-colorlight-mac.sh --test-pattern
```

The default generates 30 fps content at the configured 60 Hz output cadence, deliberately sending each complete image twice. For a new image on every refresh:

```sh
./tools/run-colorlight-mac.sh --test-pattern --test-fps=60
```

`--test-fps` accepts integers 1–60, cannot exceed the output refresh rate, and requires `--test-pattern`. The test needs at least 384×32 pixels. Colorlight test mode uses native receiver coordinates, bypassing the input `flip_x`/`flip_y` settings; the current live source needs horizontal reversal but the locally drawn counter does not. Normal playback retains its configured flips. It opens no NightDriver input socket, so incoming live content cannot replace the pattern. Ctrl-C stops it; run the launcher without test flags to resume ordinary NDSCPP input. When the RAM profile is already active, `./ndpi --config=examples/colorlight-mac-512x32.json --test-pattern` starts the test directly without reapplying configuration.

- **F00000000:** decimal content-frame ID. At the default 30/60 setting it should appear on two consecutive refreshes; at 60/60 it advances every refresh.
- **30 FPS / 60 HZ:** selected content and output rates.
- **S0000:** cumulative missed host refresh ticks. A nonzero value means the test scheduler itself missed deadlines; IDs skip rather than being sent in catch-up bursts.
- **Cyan code bars:** the low 16 frame-ID bits, most significant on the left. Cyan is one and dim blue is zero. The same code is drawn at the top and bottom for slow-motion inspection. Rolling-shutter camera effects can also produce mixed codes, so a mismatch alone does not establish a receiver fault.
- **White markers:** advance one pixel per content frame, aligned in upper/lower lanes. **Orange comet:** advances four pixels per frame. **Purple diamond:** advances three pixels per frame and bounces vertically.
- **Fixed gray panel boundaries and colored strip:** mark each 64-pixel section to reveal repeats, mirroring, and wraps.

The mode derives frame IDs from scheduled ticks, counts missed ticks, and leaves identical frame bytes untouched on repeated refreshes. Its 30/60 hardware capture decoded IDs `0,0,1,1,...,59,59` across 120 refreshes, confirmed agreement of both binary codes, and verified equality of every repeated complete-frame hash. Unit tests also check 60/60 content changes.


### Slow-motion observation of uneven presentation

The user's `IMG_4025.mov` retains a 240 fps slow-motion section retimed to 30 fps playback. In a manually checked event at playback 20.933–21.733 seconds, frame IDs 19088, 19089, and 19090 occupy 12, 4, and 8 stored camera frames respectively: approximately 50, 16.7, and 33.3 ms of capture time. Both binary-code rows agree on all 24 camera frames, with strong lit/unlit separation; the decimal counter agrees at the checked boundaries and inside the extended hold. This supports an extra 60 Hz visible hold followed by catch-up. Measurement resolution is approximately 4.17 ms.

The same uninterrupted test process's emitted missed-host-tick counter still reads `S0000`. No contemporaneous Ethernet capture was retained for the video, so this observation and prior clean transmission audits support, but do not conclusively establish, receiver-side latch/scanout timing as the cause. Other automated readings affected by transition blur/PWM are excluded from the verified event. The exact latch behavior remains unresolved; the original 30 fps native-orientation diagnostic continues running without sender changes.


### Preparing pixels before the sync deadline

The current candidate timing change separates pixel upload from display synchronization. With `prepare_ahead_us: 2000`, the scheduler wakes about 2 ms before the intended latch, selects the newest buffered frame due at that latch time, and sends its pixel packets. It retains the 500 µs minimum settling guard, then waits until the absolute deadline to send synchronization. Retained-image refreshes use the same deadline. A value of zero preserves uploading after the tick begins.

Colorlight cadence now uses nanosecond periods (16,666,666 ns at 60 Hz), avoiding the former whole-microsecond rate error. On macOS, only the final sync wait uses a bounded 100 µs spin after the Mach sleep, at most 0.6% of one core at 60 Hz. HUB75 keeps its existing VSync path.

A one-minute, 3,600-refresh diagnostic capture with this change measured sync intervals of 16.629–16.708 ms, with 0.0095 ms standard deviation, versus 16.271–17.049 ms and 0.1372 ms previously. New-content intervals were 33.297–33.369 ms; every content ID occupied exactly two refreshes, and the host missed-tick count stayed zero. The user reported that diagnostic stalls were improved or gone with this timing change. A startup-blue hardware check also passed at 16.651–16.689 ms intervals. A live capture subsequently verified all 600 output refreshes against incoming pixels, with exactly two refreshes per image and no skipped or backward source steps. Live sync intervals were 16.638–16.698 ms and image-change intervals were 33.302–33.360 ms. The user also reported stalls improved or gone in normal live ticker playback. A follow-up optical recording would establish whether every visible hold is now uniform.

For a time-correlated diagnostic log, pipe an Ethernet pcap stream into `tools/colorlight_pattern_timing.py CONFIG - OUTPUT.csv`. It decodes the complete native test-pattern frame number, missed-host-tick count, frame hash, sync timestamp, and pixel-to-sync gap. This permits a recorded physical frame ID to be matched to its exact outgoing sync timing. The tool expects the built-in native-orientation pattern and single-copy pixels/synchronization.
