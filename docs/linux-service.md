# Linux service for Colorlight output

NightDriver-Pi receives NDSCPP frames over TCP (49152 in this example). It sends the
Colorlight receiver's temporary setup profile and pixel stream over raw Ethernet,
without an IP address on the receiver-facing interface. If both applications run
on one host, NDSCPP can connect to `127.0.0.1:49152`.

## Build and install

Build from a checked-out source tree:

```sh
make -j4 WITH_HUB75=0 all ram-config test
install -d -m 0755 /opt/ndpi/bin /etc/ndpi
install -m 0755 ndpi /opt/ndpi/bin/ndpi
install -m 0755 build/tools/colorlight-ram-config /opt/ndpi/bin/colorlight-ram-config
install -m 0755 tools/apply-colorlight-ram.sh /opt/ndpi/bin/apply-colorlight-ram.sh
install -m 0644 deploy/ndpi.service /etc/systemd/system/ndpi.service
```

For a fresh installation, copy the [512×32 example](../examples/colorlight-linux-512x32.json)
to `/etc/ndpi/colorlight.json` and adapt it before starting. Keep existing site
configuration when upgrading. Stop the service before replacing installed binaries.
Install the receiver's audited local RAM bundle at
`/etc/ndpi/fm6124-512x32-runtime-complete.ndram` (or set `NDPI_RAM_BUNDLE` in a unit
drop-in), making it readable by the service's dynamic account. The bundle is not
distributed with source; do not substitute a profile from a different receiver.

Set `output.interface` to the dedicated receiver-facing interface. The unit's
`NDPI_CONFIG` points both the startup helper and playback at the same JSON file;
there is no separate interface in the unit. An older `NDPI_INTERFACE` override
is obsolete once the new unit, helper, and RAM tool are installed together.
If using a VLAN, confirm both profile and pixel frames stay on the isolated
segment; leave management/TCP connectivity on its existing interface. Do not
change global flooding settings as a substitute for isolation.

```sh
/opt/ndpi/bin/colorlight-ram-config --packets /etc/ndpi/fm6124-512x32-runtime-complete.ndram --config /etc/ndpi/colorlight.json
systemd-analyze verify /etc/systemd/system/ndpi.service
systemctl daemon-reload
systemctl enable --now ndpi.service
```

The first command is a dry run: it validates the profile and configuration
without opening any network interface. Service startup sends the RAM profile
three times, with 100 ms between packets and a pause between passes. It does
not write flash or EEPROM. A send log is not a receiver acknowledgement; check
the physical image after deployment and recovery.

## Recover after receiver power loss

Once the receiver has booted and link is up, restart only the Colorlight output:

```sh
systemctl restart ndpi.service
systemctl is-active ndpi.service
journalctl -u ndpi -n 50 --no-pager
```

NDSCPP need not restart. If the picture is still wrong, check that the dedicated
interface carries both the setup and pixel traffic and the receiver has the right
profile. Do not broaden multicast forwarding into the management LAN.

## Timing

On one shared CPU, a 2 ms row-prepare lead was shorter than upload plus the
guard time. The Linux example prepares rows 8 ms ahead of the latch deadline.
The service uses CPU weight and reduced timer slack rather than relying on
real-time scheduling privileges. Adjust timing based on captures: compare
input TCP images with complete output frames, and confirm the actual display.

```sh
python3 tools/colorlight_capture_audit.py /etc/ndpi/colorlight.json input.pcap output.pcap report 49152
```

Start the input capture at least two seconds before the Ethernet capture to
include already-buffered frames. Keep captures local.
